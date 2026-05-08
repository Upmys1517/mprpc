#include <iostream>
#include <string>

#include "mprpcapplication.h"
#include "rpcprovider.h"
#include "user.pb.h"
#include "logger.h"

/*
*UserService原来是一个本地服务，提供两个进程内的本地方法:Login和GetFriendLists
*/
class UserService : public fixbug::UserServiceRpc {
public:
    bool Login(std::string username, std::string password){
        std::cout << "UserService::Login" << std::endl;
        std::cout << "username: " << username << std::endl;
        std::cout << "password: " << password << std::endl;
        return true;
    }

    bool Register(uint32_t id, std::string username, std::string password) {
        std::cout << "UserService::Register" << std::endl;
        std::cout << "username: " << id << std::endl;
        std::cout << "username: " << username << std::endl;
        std::cout << "password: " << password << std::endl;
        return true;
    }

    //重写基类UserServiceRpc的虚函数Login,这个函数是框架直接调用的
    //框架在调用Login函数时会自动将网络上收到的LoginRequest参数反序列化成LoginRequest类型的参数传给Login函数
    void Login(::google::protobuf::RpcController* controller,
               const ::fixbug::LoginRequest* request,
               ::fixbug::LoginResponse* response,
               ::google::protobuf::Closure* done) override {
        
        //框架给业务上报了请求参数LoginRequest, 应用获取相应数据做做本地业务
        std::string username = request->username();
        std::string password = request->password();

        //做本地业务
        bool login_result = Login(username, password);

        //把响应写入LoginResponse(包括错误码，错误信息和登陆结果),框架会自动把LoginResponse序列化后发回给请求方
        fixbug::ResultCode* code = response->mutable_result();
        code->set_errcode(0);
        code->set_errmsg("");
        response->set_success(login_result);

        //执行回调操作,框架会调用这个回调操作,把响应写回到网络上发给请求方
        done->Run();
    }

    void Register(::google::protobuf::RpcController* controller,
               const ::fixbug::RegisterRequest* request,
               ::fixbug::RegisterResponse* response,
               ::google::protobuf::Closure* done) override {
        uint32_t id = request->id();
        std::string username = request->username();
        std::string password = request->password();

        bool ret = Register(id, username, password);

        response->mutable_result()->set_errcode(0);
        response->mutable_result()->set_errmsg("");
        response->set_success(ret);

        done->Run();
    }
};

int main(int argc, char** argv){
    //调用框架的初始化操作
    MprpcApplication::Init(argc, argv);

    //provider是一个rpc网络服务对象,把UserService对象发布到rpc节点上,让rpc节点可以调用到这个服务
    RpcProvider provider;
    provider.NotifyService(new UserService());

    //启动一个rpc服务发布结点，Run以后进程进入阻塞状态，等待远程的rpc调用请求
    provider.Run();

    return 0;
}