#pragma once

#include <google/protobuf/service.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <string>

#include "zookeeperutil.h"

class MprpcChannel : public google::protobuf::RpcChannel {
public:
    explicit MprpcChannel(bool connectNow);
    ~MprpcChannel() override;

    //所有通过stub代理对象调用的rpc方法，都走到这里了，统一做rpc方法调用的数据序列化和网络发送
    void CallMethod(const google::protobuf::MethodDescriptor* method,
                          google::protobuf::RpcController* controller,
                          const google::protobuf::Message* request,
                          google::protobuf::Message* response,
                          google::protobuf::Closure* done) override;

private:
    //解析服务地址：优先查本地缓存，未命中再问 zookeeper
    bool resolveEndpoint(const std::string& method_path,
                         std::string& ip,
                         uint16_t& port);

    //归还连接：channel 析构时把连接放回进程级连接池，下次调用直接复用
    void releaseConnection();

    //丢弃连接：收发或解析失败后连接状态不可信，直接关闭，不放回池里
    void discardConnection();

    int m_clientfd = -1;
    std::string m_ip;
    uint16_t m_port = 0;
};
