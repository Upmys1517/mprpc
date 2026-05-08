#pragma once

#include "mprpcconfig.h"
#include "mprpcchannel.h"
#include "mprpccontroller.h"

//mprpc框架的基础类，提供一些框架的基本功能
class MprpcApplication {
public:
    static void Init(int argc, char** argv);
    //获取mprpc框架的全局唯一实例对象(单例模式)
    static MprpcApplication& Getinstance();
    static MprpcConfig& GetConfig();//获取配置对象
private:
    static MprpcConfig m_config;//配置类对象
    
    MprpcApplication() {}
    MprpcApplication(const MprpcApplication&) = delete;//禁止拷贝构造函数
    MprpcApplication(MprpcApplication&&) = delete;//禁止移动构造函数
    MprpcApplication& operator=(const MprpcApplication&) = delete;//禁止赋值构造函数
    MprpcApplication& operator=(MprpcApplication&&) = delete;//禁止移动赋值构造函数

};