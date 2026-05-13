#include "mprpcchannel.h"
#include "rpcheader.pb.h"
#include "mprpcapplication.h"
#include "mprpccontroller.h"
#include "logger.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mutex>

std::mutex g_data_mutex;//互斥锁，保证线程安全


/*
*header_size(4字节) + header_str(service_name, method_name, args_size) + args_str
*1.先序列化业务数据 args_str，并计算出它的长度。
*2.然后用这个长度值去创建 RpcHeader，并序列化它。
*3.最后，按照“长度+头部+业务数据”的顺序，把它们拼接成最终要发送的字节流。
 */
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor *method,
                google::protobuf::RpcController *controller,
                const google::protobuf::Message *request,
                google::protobuf::Message *response,
                google::protobuf::Closure *done) {
    const google::protobuf::ServiceDescriptor* sd = method->service();
    service_name = sd->name();
    method_name = method->name();

    //获取参数化字符串长度args_size
    uint32_t args_size = 0;
    std::string args_str;
    if(request->SerializeToString(&args_str)) {
        args_size = args_str.size();
    }else {
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
    if(rpc_header.SerializeToString(&rpc_header_str)) {
        header_size = rpc_header_str.size();
    }else {
        controller->SetFailed("serialize rpc header error!");
        return;
    }

    //组织最终发送的字符串
    std::string send_rpc_str;
    send_rpc_str.insert(0, std::string((char*)&header_size, 4));
    send_rpc_str += rpc_header_str;
    send_rpc_str += args_str;


    //读取配置文件rpcserver信息
    // std::string ip = MprpcApplication::Getinstance().GetConfig().Load("rpcserverip");
    // uint16_t port = atoi(MprpcApplication::Getinstance().GetConfig().Load("rpcserverport").c_str());

    //rpc调用方想调用service_name的method_name服务，需要查询zk上该服务所在的host信息
    ZkClient zkCli;
    zkCli.Start();
    //  /UserServiceRpc/Login
    std::string host_data = QueryServiceHost(&zkCli, service_name, method_name, m_idx);
    m_ip = host_data.substr(0, m_idx);
    m_port = atoi(host_data.substr(m_idx + 1, host_data.size() - m_idx).c_str());

    bool rt = newConnect(m_ip.c_str(), m_port);
    if(!rt) {
        LOG_ERR("connect server error!");
    } else {
        LOG_INFO("connect server success!");
    }

    //发送rpc请求
    if(send(m_clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0) == -1) {
        close(m_clientfd);
        char errtext[512] = {0};
        sprintf(errtext, "send error! errno: %d", errno);
        controller->SetFailed(errtext);
        return;
    }

    //接收rpc请求的响应值
    char recv_buf[1024] = {0};
    int recv_size = recv(m_clientfd, recv_buf, 1024, 0);
    if(recv_size == -1) {
        close(m_clientfd);
        char errtext[512] = {0};
        sprintf(errtext, "recv error! errno: %d", errno);
        controller->SetFailed(errtext);
        return;
    }

    //反序列化rpc调用的响应数据
    //std::string response_str(recv_buf, 0, recv_size);//出现问题：recv_buf中遇到\0，后面的数据就存不下来了，反序列化失败
    //if(!response->ParseFromString(response_str)) 
    if(!response->ParseFromArray(recv_buf, recv_size)) {
        close(m_clientfd);
        char errtext[1096] = {0};
        sprintf(errtext, "parse error! response_str: %s", recv_buf);
        controller->SetFailed(errtext);
        return;
    }  
    close(m_clientfd);
}

bool MprpcChannel::newConnect(const char* ip, uint16_t port) {
    //创建socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if(clientfd == -1) {
        char errtext[512] = {0};
        sprintf(errtext, "create socket error! errno: %d", errno);
        LOG_ERR("%s", errtext);
        return false; 
    }

    //设置服务器地址信息
    struct sockaddr_in server_addr;
    socklen_t len = sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    //连接rpc服务结点
    if(connect(clientfd, (struct sockaddr*)&server_addr, len) == -1) {
        close(clientfd);
        char errtext[512] = {0};
        sprintf(errtext, "connect error! errno: %d", errno);
        LOG_ERR("%s", errtext);
        return false;
    }

    m_clientfd = clientfd;
    return true;
}

std::string MprpcChannel::QueryServiceHost(ZkClient* zkclient, std::string service_name, std::string mrthod_name, int &idx) {
    std::string method_path = "/" + service_name + "/" + method_name;

    std::unique_lock<std::mutex> lock(g_data_mutex);
    std::string host_data_1 = zkclient->GetData(method_path.c_str());
    lock.unlock();

    if(host_data_1 == "") {
        LOG_ERR("%s is not exist!", method_path.c_str());
        return "";
    }
    idx = host_data_1.find(":");
    if(idx == -1) {
        LOG_ERR("%s address is invalid!", method_path.c_str());
        return "";
    }
    return host_data_1;
}

//构造函数，支持延迟连接
MprpcChannel::MprpcChannel(bool connectNow) : m_clientfd(-1), m_idx(0) {
    if(!connectNow) {
        return;
    }

    //尝试连接服务器，最多尝试三次
    bool rt = newConnect(m_ip.c_str(), m_port);
    int count = 3;
    while(!rt && count--){
        rt = newConnect(m_ip.c_str(), m_port);
    }
}