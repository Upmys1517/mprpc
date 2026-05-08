#include "zookeeperutil.h"
#include "mprpcapplication.h"
#include <iostream>
// #include <mutex>
// #include <condition_variable>
#include "logger.h"


std::mutex m_mutex;
std::condition_variable m_cond;
bool is_connected = false;

// 全局的watcher观察器，用于接收zookeeper服务器的通知
void global_watcher(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {
    if(type == ZOO_SESSION_EVENT) {//回调消息类型是和会话相关的消息类型
        if(state == ZOO_CONNECTED_STATE) {//zkclient与zkserver连接成功
            sem_t *sem = (sem_t*)zoo_get_context(zh);
            sem_post(sem);//信号量加1，唤醒等待线程

            // std::lock_guard<std::mutex> lock(m_mutex);
            // is_connected = true;
            // m_cond.notify_one();
        }
    }
}

ZkClient::ZkClient() : m_zhandle(nullptr) {}

ZkClient::~ZkClient() {
    if(m_zhandle) {
        zookeeper_close(m_zhandle);
    }
}

/*
源码上会在三分之一的Timeout时间发送ping心跳消息
zookeeper_mt: 多线程版本
zookeeper的API客户端程序提供了三个线程
API调用线程
网络I/O线程 pthread_create poll
watcher回调线程
*/
void ZkClient::Start() {
    std::string host = MprpcApplication::GetConfig().Load("zookeeperip");
    std::string port = MprpcApplication::GetConfig().Load("zookeeperport");
    std::string connstr = host + ":" + port;

    //使用zookeeper_init初始化一个ZooKeeper客户端对象，异步建立与服务器的连接
    m_zhandle = zookeeper_init(connstr.c_str(), global_watcher, 30000, nullptr, nullptr, 0);
    if(m_zhandle == nullptr) {
        LOG_ERR("zookeeper_init error!");
        exit(EXIT_FAILURE);
    }

    sem_t sem;
    //初始化信号量 第一个0(pshared)代表线程间共享，非0表示进程间共享
    //第二个0(value)代表信号量的初始值（通常表示可用资源数量）
    sem_init(&sem, 0, 0);
    zoo_set_context(m_zhandle, &sem);

    sem_wait(&sem);//信号量减1，如果为0则阻塞

    //等待连接完成
    // std::unique_lock<std::mutex> lock(m_mutex);
    // m_cond.wait(lock, [this](){
    //     return is_connected;
    // });

    LOG_INFO("zookerrper_init success!");
}

void ZkClient::Create(const char *path, const char *data, int datalen, int state) {
    char path_buffer[128];
    int bufferlen = sizeof(path_buffer);
    int flag = zoo_exists(m_zhandle, path, 0, nullptr);

    if(flag == ZNONODE) {//ZNONODE表示结点不存在
        //创建结点
        flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
        if(flag == ZOK) {//ZOK表示创建成功
            LOG_INFO("znode create success... path: %s", path);
        } else {
            LOG_ERR("znode create error! flag: %d, path: %s", flag, path);
            exit(EXIT_FAILURE);
        }
    }
}

std::string ZkClient::GetData(const char *path) {
    char buffer[64];
    int bufferlen = sizeof(buffer);
    int flag = zoo_get(m_zhandle, path, 0, buffer, &bufferlen, nullptr);
    if(flag != ZOK) {
        LOG_ERR("get znode error... path: %s", path);
        return "";
    }else {
        return buffer;
    }
}
