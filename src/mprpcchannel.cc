#include "mprpcchannel.h"
#include "rpcheader.pb.h"
#include "mprpcapplication.h"
#include "mprpccontroller.h"
#include "logger.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/*
header_size(4字节) + header_str(service_name, method_name, args_size) + args_str
1.先序列化业务数据 args_str，并计算出它的长度。
2.然后用这个长度值去创建 RpcHeader，并序列化它。
3.最后，按照“长度+头部+业务数据”的顺序，把它们拼接成最终要发送的字节流。

响应侧同样带 4 字节长度前缀，这样长连接上可以精确读满一条响应，不会因为一次 recv
读到半个包而错位。
 */
namespace {

//空闲连接池：每个 endpoint 最多缓存这么多条连接，超出直接关闭
constexpr size_t kMaxIdlePerEndpoint = 16;
constexpr int    kMaxAttempts        = 2;   //池里的连接可能已被对端关掉，失败后重连重试一次

std::mutex g_endpoint_mutex;
std::unordered_map<std::string, std::string> g_endpoint_cache;  // "/Service/Method" -> "ip:port"

std::mutex g_pool_mutex;
std::unordered_map<std::string, std::vector<int>> g_idle_conns;  // "ip:port" -> clientfd

std::mutex g_zk_mutex;

//整个进程共用一个 zookeeper 会话：注册中心只用来发现地址，没必要每次调用都重建会话
ZkClient& sharedZkClient() {
    static ZkClient zk;
    static std::once_flag once;
    std::call_once(once, []() { zk.Start(); });
    return zk;
}

int connectOnce(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERR("create socket error! errno: %d", errno);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (::connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        LOG_ERR("connect %s:%u error! errno: %d", ip.c_str(), port, errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

//取连接：优先复用池里的空闲连接，池空才真正建连
int acquireConnection(const std::string& ip, uint16_t port) {
    const std::string key = ip + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        auto it = g_idle_conns.find(key);
        if (it != g_idle_conns.end() && !it->second.empty()) {
            int fd = it->second.back();
            it->second.pop_back();
            return fd;
        }
    }
    return connectOnce(ip, port);
}

void releaseConnectionToPool(const std::string& ip, uint16_t port, int fd) {
    if (fd < 0) {
        return;
    }
    const std::string key = ip + ":" + std::to_string(port);
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    auto& idle = g_idle_conns[key];
    if (idle.size() < kMaxIdlePerEndpoint) {
        idle.push_back(fd);
        return;
    }
    ::close(fd);
}

bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool recvExact(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        return false;   // 0 表示对端关闭，-1 表示出错
    }
    return true;
}

} // namespace

//构造函数，支持延迟连接
MprpcChannel::MprpcChannel(bool connectNow) : m_clientfd(-1), m_port(0) {
    //地址要等服务名/方法名确定后才能解析，这里拿不到 endpoint，
    //真正的建连推迟到第一次 CallMethod。
    (void)connectNow;
}

MprpcChannel::~MprpcChannel() {
    releaseConnection();
}

void MprpcChannel::releaseConnection() {
    if (m_clientfd < 0) {
        return;
    }
    releaseConnectionToPool(m_ip, m_port, m_clientfd);
    m_clientfd = -1;
}

bool MprpcChannel::resolveEndpoint(const std::string& method_path,
                                   std::string& ip,
                                   uint16_t& port) {
    std::string host_data;

    {
        std::lock_guard<std::mutex> lock(g_endpoint_mutex);
        auto it = g_endpoint_cache.find(method_path);
        if (it != g_endpoint_cache.end()) {
            host_data = it->second;
        }
    }

    if (host_data.empty()) {
        std::lock_guard<std::mutex> lock(g_zk_mutex);
        host_data = sharedZkClient().GetData(method_path.c_str());
        if (host_data.empty()) {
            LOG_ERR("%s is not exist!", method_path.c_str());
            return false;
        }
        std::lock_guard<std::mutex> cache_lock(g_endpoint_mutex);
        g_endpoint_cache[method_path] = host_data;
    }

    const size_t idx = host_data.find(':');
    if (idx == std::string::npos) {
        LOG_ERR("%s address is invalid!", method_path.c_str());
        return false;
    }

    ip = host_data.substr(0, idx);
    port = static_cast<uint16_t>(atoi(host_data.substr(idx + 1).c_str()));
    return port != 0;
}

void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                google::protobuf::RpcController* controller,
                const google::protobuf::Message* request,
                google::protobuf::Message* response,
                google::protobuf::Closure* done) {
    const google::protobuf::ServiceDescriptor* sd = method->service();
    const std::string service_name = sd->name();
    const std::string method_name  = method->name();

    //获取参数化字符串长度args_size
    uint32_t args_size = 0;
    std::string args_str;
    if (request->SerializeToString(&args_str)) {
        args_size = static_cast<uint32_t>(args_str.size());
    } else {
        controller->SetFailed("serialize request error!");
        return;
    }

    //定义rpc的请求header
    mprpc::RpcHeader rpc_header;
    rpc_header.set_service_name(service_name);
    rpc_header.set_method_name(method_name);
    rpc_header.set_args_size(args_size);

    uint32_t header_size = 0;
    std::string rpc_header_str;
    if (rpc_header.SerializeToString(&rpc_header_str)) {
        header_size = static_cast<uint32_t>(rpc_header_str.size());
    } else {
        controller->SetFailed("serialize rpc header error!");
        return;
    }

    //组织最终发送的字符串
    std::string send_rpc_str;
    send_rpc_str.reserve(4 + header_size + args_size);
    send_rpc_str.append(reinterpret_cast<const char*>(&header_size), 4);
    send_rpc_str += rpc_header_str;
    send_rpc_str += args_str;

    //服务地址走本地缓存，只有首次（或缓存被清空）才去 zookeeper 查
    const std::string method_path = "/" + service_name + "/" + method_name;
    if (m_clientfd < 0 && (m_ip.empty() || m_port == 0)) {
        if (!resolveEndpoint(method_path, m_ip, m_port)) {
            controller->SetFailed("resolve service host error!");
            return;
        }
    }

    std::string err_text;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        //第一次优先复用本 channel 手里的连接，否则从池里取
        if (m_clientfd < 0) {
            m_clientfd = acquireConnection(m_ip, m_port);
            if (m_clientfd < 0) {
                err_text = "connect server error!";
                continue;
            }
        }

        if (!sendAll(m_clientfd, send_rpc_str.data(), send_rpc_str.size())) {
            err_text = "send error!";
            releaseConnection();      // 不把坏连接放回池里
            if (attempt + 1 < kMaxAttempts) {
                continue;
            }
            controller->SetFailed(err_text);
            return;
        }

        uint32_t resp_size = 0;
        if (!recvExact(m_clientfd, reinterpret_cast<char*>(&resp_size), 4)
                || resp_size == 0) {
            err_text = "recv response size error!";
            releaseConnection();
            if (attempt + 1 < kMaxAttempts) {
                continue;
            }
            controller->SetFailed(err_text);
            return;
        }

        std::string response_str(resp_size, '\0');
        if (!recvExact(m_clientfd, &response_str[0], resp_size)) {
            err_text = "recv response body error!";
            releaseConnection();
            if (attempt + 1 < kMaxAttempts) {
                continue;
            }
            controller->SetFailed(err_text);
            return;
        }

        if (!response->ParseFromString(response_str)) {
            controller->SetFailed("parse error! response_str: " + response_str);
            releaseConnection();
            return;
        }

        //成功：连接留在本 channel 上，析构时归还连接池
        return;
    }

    controller->SetFailed(err_text.empty() ? "rpc call failed!" : err_text);
}
