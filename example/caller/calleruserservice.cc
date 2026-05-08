#include <iostream>

#include "mprpcapplication.h"
#include "user.pb.h"

int main(int argc, char** argv) {
    //整个程序启动以后，想使用mprpc框架来享受rpc服务调用，一定要先调用框架的初始化函数（只初始化一次）
    MprpcApplication::Init(argc, argv);

    //演示调用远程发布的rpc方法Login
    fixbug::UserServiceRpc_Stub stub(new MprpcChannel(false));
    //rpc方法的请求参数
    fixbug::LoginRequest request;
    request.set_username("Zhang San");
    request.set_password("123456");

    //rpc方法的响应
    fixbug::LoginResponse response;

    MprpcController controller;

    //发起rpc方法的调用，等待返回结果
    stub.Login(&controller, &request, &response, nullptr);

    //一次rpc调用完成，读取结果
    if(controller.Failed()) {
        std::cout << controller.ErrorText() << std::endl;
    } else {
        if(!response.result().errcode()) {
            std::cout << "rpc login response success: " << response.success() << std::endl;
        } else {
            std::cout << "rpc login response error: " << response.result().errmsg() << std::endl;
        }
    }
    

    //演示调用远程发布的rpc方法Register
    fixbug::RegisterRequest req;
    req.set_id(2000);
    req.set_username("mprpc");
    req.set_password("666666");
    fixbug::RegisterResponse rsp;

    //以同步的方式发起rpc调用请求，等待返回结果
    stub.Register(&controller, &req, &rsp, nullptr);

    //一次rpc调用完成，读取结果
    if(controller.Failed()) {
        std::cout << controller.ErrorText() << std::endl;
    } else {
        if(!rsp.result().errcode()) {
            std::cout << "rpc Register response success: " << rsp.success() << std::endl;
        } else {
            std::cout << "rpc Register response error: " << rsp.result().errmsg() << std::endl;
        }
    }

    return 0;
}