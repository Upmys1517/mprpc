#pragma once

#include "google/protobuf/service.h"
#include "muduo/net/TcpServer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/TcpConnection.h"
#include "mprpcapplication.h"
#include "google/protobuf/descriptor.h"
#include "logger.h"
#include "zookeeperutil.h"

#include <string>
#include <functional>
#include <unordered_map>



//框架提供的专门用于发布rpc服务的类，RpcProvider是一个rpc服务发布者，负责把外部业务代码和rpc框架进行连接
class RpcProvider {
public:
    //这是框架提供给外部使用的，可以发布rpc方法的函数接口
    void NotifyService(google::protobuf::Service* service);

    //启动rpc服务发布节点，开始提供rpc远程网络调用服务
    void Run();

private:
    //muduo网络库编程，EventLoop事件循环
    muduo::net::EventLoop m_eventLoop;

    //服务类型信息
    struct ServiceInfo{
        //保存服务对象
        google::protobuf::Service* m_service;
        //保存服务方法的描述信息 key是方法名字，value是方法描述信息
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> m_methodMap;
    };

    //保存注册成功的服务对象和服务方法的所有信息，key是服务名字，value是服务信息
    std::unordered_map<std::string, ServiceInfo> m_serviceMap;

    //新的socket连接回调
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    //已建立连接用户的读写事件回调, 如果远程有rpc调用请求，就会自动调用这个onMessage方法
    void onMessage(const muduo::net::TcpConnectionPtr& conn
                    , muduo::net::Buffer* buffer
                    , muduo::Timestamp receiveTime);
    
    //Closure的回调操作，用于序列化rpc的响应和网络发送
    void SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, google::protobuf::Message* response);
};