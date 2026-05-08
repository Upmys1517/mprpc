#pragma once

#include <string>
#include <unordered_map>

//框架读取配置文件
//rpcserver ip
//rpcserver port
//zookeeper ip
//zookeeper port
class MprpcConfig {
public:
    //负责解析加载配置文件
    void LoadConfigFile(const char* config_file);
    //查询配置项信息
    std::string Load(const std::string& key);
private:
    std::unordered_map<std::string, std::string> m_configMap;//存储配置文件中的参数

    //去掉字符串前后的空格
    void Trim(std::string& src_buf);
};