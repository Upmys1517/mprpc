#include "rpcprovider.h"
#include "rpcheader.pb.h"
#include <chrono>
#include <cstdio>
#include <cstring>

void RpcProvider::NotifyService(google::protobuf::Service *service) {
    ServiceInfo service_info;
    service_info.m_service = service;

    //获取了服务对象的描述信息
    const google::protobuf::ServiceDescriptor* pserviceDesc = service->GetDescriptor();
    //获取服务对象的名字
    std::string service_name = pserviceDesc->name();
    //获取服务对象的服务方法的数量
    int methodCnt = pserviceDesc->method_count();

    //std::cout << "service_name: " << service_name << std::endl;
    LOG_INFO("service_name: %s", service_name.c_str());

    for(int i = 0; i < methodCnt; ++i) {
        //获取了服务对象指定下标的服务方法的描述信息
        const google::protobuf::MethodDescriptor* pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        service_info.m_methodMap.insert({method_name, pmethodDesc});

        //std::cout << "method_name: " << method_name << std::endl;
        LOG_INFO("method_name: %s", method_name.c_str());
    }
    //将服务信息插入到服务映射表中
    m_serviceMap.insert({service_name, service_info});
}

void RpcProvider::Run() {
    std::string ip = MprpcApplication::GetConfig().Load("rpcserverip");
    uint16_t port = atoi(MprpcApplication::GetConfig().Load("rpcserverport").c_str());
    muduo::net::InetAddress address(ip, port);

    //创建TcpServer对象
    muduo::net::TcpServer server(&m_eventLoop, address, "RpcProvider");

    //绑定连接回调函数和读写消息回调方法
    server.setConnectionCallback(std::bind(&RpcProvider::onConnection, this, std::placeholders::_1));
    server.setMessageCallback(std::bind(&RpcProvider::onMessage, this
        , std::placeholders::_1
        , std::placeholders::_2
        , std::placeholders::_3));

    //设置muduo库的线程数量，muduo库会自己分配I/O线程和worker线程
    server.setThreadNum(4);

    // 把当前rpc节点上要发布的服务全部注册到zkserver上面，让rpc client 可以从zkserver上发现服务
    ZkClient zkCli;
    zkCli.Start();
    //server_name 为永久性节点  method_name为临时性结点
    for(const auto& sp : m_serviceMap) {
        // /server_name eg: /UserServiceRpc
        std::string service_path = "/" + sp.first;
        if (!zkCli.Create(service_path.c_str(), nullptr, 0)) {
            //日志是异步写线程，exit 前不一定落盘，致命错误同步打到 stderr
            LOG_ERR("register service %s failed, provider exit", service_path.c_str());
            fprintf(stderr, "register service %s to zookeeper failed, provider exit\n", service_path.c_str());
            exit(EXIT_FAILURE);
        }
        for(const auto& mp : sp.second.m_methodMap) {
            // /service_name/method_name  eg: /UserServiceRpc/Login 存储当前这个rpc服务节点的主机ip port
            std::string method_path = service_path + "/" + mp.first;
            char method_path_data[128] = { 0 };
            sprintf(method_path_data, "%s:%d", ip.c_str(), port);
            // ZOO_EPHEMERAL表示znode是一个临时性结点
            //注册失败时服务对客户端不可见，继续运行只会让调用方一直报"服务不存在"，直接退出
            if (!zkCli.Create(method_path.c_str(), method_path_data, sizeof(method_path_data), ZOO_EPHEMERAL)) {
                LOG_ERR("register method %s failed, provider exit", method_path.c_str());
                fprintf(stderr, "register method %s to zookeeper failed, provider exit\n", method_path.c_str());
                exit(EXIT_FAILURE);
            }
        }
    }

    //std::cout << "RpcProvider start service at " << ip << ":" << port << std::endl;
    LOG_INFO("RpcProvider start service at %s : %d", ip.c_str(), port);

    //启动服务
    server.start();
    //进入事件循环，等待客户端的连接和请求
    m_eventLoop.loop();
}

void RpcProvider::onConnection(const muduo::net::TcpConnectionPtr& conn) {
    if(!conn->connected()) {
        //如果断开连接了，就把这个连接从连接表中删除掉
        conn->shutdown();
    }
}


/*
*1.先从前4个字节里读出 header_size（例如30）
*2.然后根据 header_size (30) 取出后面的30个字节，这正好是整个 RpcHeader。
*3.接着从 RpcHeader 里得知 args_size (比如20)，然后取出最后的20个字节，这恰好是整个 args_str。
*4.最后才对这个 args_str 执行 ParseFromString，还原出 LoginRequest 对象。
*/
void RpcProvider::onMessage(const muduo::net::TcpConnectionPtr& conn
                    , muduo::net::Buffer* buffer
                    , muduo::Timestamp receiveTime) {
    //长连接下，一次读事件里可能是一个完整请求、半个请求，也可能是多个请求粘在一起：
    //按 header_size + args_size 切帧，凑不满就等下一次读事件
    while (true) {
        const size_t readable = buffer->readableBytes();
        if (readable < 4) {
            return;
        }

        //从字符流中读取前4个字节的内容，换成一个int_t类型的整数，整数的值就是rpc请求头的长度
        uint32_t header_size = 0;
        memcpy(&header_size, buffer->peek(), 4);
        if (readable < 4 + static_cast<size_t>(header_size)) {
            return;
        }

        //根据header_size读取数据头的原始字符流，反序列化数据头，获取rpc请求的详细信息
        std::string rpc_header_str(buffer->peek() + 4, header_size);
        mprpc::RpcHeader rpc_header;
        if (!rpc_header.ParseFromString(rpc_header_str)) {
            LOG_ERR("rpc_header_str: %s parse error!", rpc_header_str.c_str());
            conn->shutdown();
            return;
        }

        const std::string service_name = rpc_header.service_name();
        const std::string method_name  = rpc_header.method_name();
        const uint32_t    args_size    = rpc_header.args_size();
        if (readable < 4 + static_cast<size_t>(header_size) + args_size) {
            return;   // 请求体还没到齐
        }

        std::string args_str(buffer->peek() + 4 + header_size, args_size);
        buffer->retrieve(4 + header_size + args_size);

        auto t_start = std::chrono::high_resolution_clock::now();

        //获取service对象和method对象
        auto it = m_serviceMap.find(service_name);
        if(it == m_serviceMap.end()) {
            LOG_ERR("%s is not exist!", service_name.c_str());
            conn->shutdown();
            return;
        }
        google::protobuf::Service* service = it->second.m_service;//获取service对象 (new UserService)

        auto mit = it->second.m_methodMap.find(method_name);
        if(mit == it->second.m_methodMap.end()) {
            LOG_ERR("%s is not exist!", method_name.c_str());
            conn->shutdown();
            return;
        }

    const google::protobuf::MethodDescriptor* method = mit->second;//获取method对象 (Login)

    //生成rpc方法调用的请求request和响应response参数
    google::protobuf::Message* request = service->GetRequestPrototype(method).New();
    if(!request->ParseFromString(args_str)) {
        LOG_ERR("request parse error! content: %s",  args_str.c_str());
        conn->shutdown();
        return;
    }
    google::protobuf::Message* response = service->GetResponsePrototype(method).New();

    auto t_after_parse = std::chrono::high_resolution_clock::now();
    int64_t parse_us = std::chrono::duration_cast<std::chrono::microseconds>(t_after_parse - t_start).count();
    int64_t t_start_us = std::chrono::duration_cast<std::chrono::microseconds>(
        t_start.time_since_epoch()).count();

    //将计时数据存入map，key为连接名，SendRpcResponse中取出计算耗时
    {
        std::lock_guard<std::mutex> lock(m_timingMutex);
        m_timingMap[conn->name()] = {t_start_us, parse_us};
    }

    //给下面的method方法的调用，绑定一个Closure回调函数
    google::protobuf::Closure* done = google::protobuf::NewCallback<RpcProvider,
                                                                    const muduo::net::TcpConnectionPtr&,
                                                                    google::protobuf::Message*>(this,
                                                                                                &RpcProvider::SendRpcResponse,
                                                                                                conn, response);

    service->CallMethod(method, nullptr, request, response, done);
    }
}

void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, google::protobuf::Message* response) {
    //从timingMap取出onMessage记录的起始时间和解析耗时
    int64_t t_start_us = 0;
    int64_t parse_us = 0;
    {
        std::lock_guard<std::mutex> lock(m_timingMutex);
        auto it = m_timingMap.find(conn->name());
        if (it != m_timingMap.end()) {
            t_start_us = it->second.first;
            parse_us = it->second.second;
            m_timingMap.erase(it);
        }
    }

    auto t_before_serialize = std::chrono::high_resolution_clock::now();

    std::string response_str;
    if(response->SerializeToString(&response_str)) {
        //把rpc方法执行的结果发送回rpc的调用方：4 字节长度前缀 + 响应体，
        //长连接下客户端据此精确读满一条响应
        uint32_t resp_size = static_cast<uint32_t>(response_str.size());
        std::string frame;
        frame.reserve(4 + response_str.size());
        frame.append(reinterpret_cast<const char*>(&resp_size), 4);
        frame += response_str;
        conn->send(frame);
    }else{
        LOG_ERR("response serialize error!");
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    int64_t response_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_before_serialize).count();
    int64_t process_us = std::chrono::duration_cast<std::chrono::microseconds>(t_before_serialize.time_since_epoch()).count() - t_start_us - parse_us;
    int64_t total_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end.time_since_epoch()).count() - t_start_us;

    LOG_INFO("[PERF] parse=%ldus process=%ldus response=%ldus total=%ldus", parse_us, process_us, response_us, total_us);

    delete response;   // 响应已经发出去，回收
    //长连接不再主动断开：连接留在连接池里给下一次调用复用
}