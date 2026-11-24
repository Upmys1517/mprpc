#include "zookeeperutil.h"
#include "mprpcapplication.h"
#include <cstring>
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

namespace {

//znode 里存的是 "ip:port" 加若干补齐的 '\0'，只比较第一个 '\0' 之前的部分
bool sameNodeData(const char* a, int alen, const char* b, int blen) {
    if (a == nullptr || b == nullptr || alen < 0 || blen < 0) {
        return false;
    }
    return std::string(a, strnlen(a, alen)) == std::string(b, strnlen(b, blen));
}

} // namespace

/*
节点已存在时不能直接当作注册成功：
- 永久节点（如 /UserServiceRpc）：已存在即可复用；
- 临时节点归属当前会话：本进程已经注册过，幂等返回；
- 临时节点归属其它会话、但地址与本进程相同：是上一次运行留下的节点，旧会话还没超时。
  RpcProvider 在注册前已经 bind 了这个端口，说明旧进程已不在监听，删掉旧节点后重建，
  否则旧会话超时后节点被 zookeeper 回收，本服务就从注册中心消失了；
- 临时节点归属其它会话、地址不同：另一个在线 provider 占用了这个方法，拒绝注册。
exists / create / delete 之间可能和旧会话超时或其它客户端并发，冲突时重新检查。
*/
bool ZkClient::Create(const char *path, const char *data, int datalen, int state) {
    constexpr int kMaxAttempts = 5;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        struct Stat stat;
        int rc = zoo_exists(m_zhandle, path, 0, &stat);

        if (rc == ZNONODE) {//ZNONODE表示结点不存在，创建结点
            char path_buffer[128];
            rc = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state,
                            path_buffer, sizeof(path_buffer));
            if (rc == ZOK) {
                LOG_INFO("znode create success... path: %s", path);
                return true;
            }
            if (rc == ZNODEEXISTS) {
                continue;   // exists 和 create 之间被别人抢先创建，重新检查归属
            }
            LOG_ERR("znode create error! rc: %d (%s), path: %s", rc, zerror(rc), path);
            return false;
        }

        if (rc != ZOK) {
            LOG_ERR("znode exists error! rc: %d (%s), path: %s", rc, zerror(rc), path);
            return false;
        }

        //节点已存在
        if ((state & ZOO_EPHEMERAL) == 0) {
            return true;
        }

        const clientid_t* self = zoo_client_id(m_zhandle);
        if (self != nullptr && stat.ephemeralOwner == self->client_id) {
            return true;
        }

        if (stat.ephemeralOwner == 0) {
            LOG_ERR("znode %s already exists as a persistent node, cannot register ephemeral node", path);
            return false;
        }

        char existing[256];
        int existing_len = sizeof(existing);
        rc = zoo_get(m_zhandle, path, 0, existing, &existing_len, &stat);
        if (rc == ZNONODE) {
            continue;   // 旧会话恰好超时，节点已被回收
        }
        if (rc != ZOK) {
            LOG_ERR("znode get error! rc: %d (%s), path: %s", rc, zerror(rc), path);
            return false;
        }

        if (!sameNodeData(existing, existing_len, data, datalen)) {
            LOG_ERR("znode %s is owned by another live session 0x%llx (data: %.*s), refuse to register",
                    path, static_cast<unsigned long long>(stat.ephemeralOwner),
                    static_cast<int>(strnlen(existing, existing_len)), existing);
            return false;
        }

        LOG_INFO("znode %s is a stale node of session 0x%llx, delete and recreate",
                 path, static_cast<unsigned long long>(stat.ephemeralOwner));
        //带版本号删除：期间节点被改过就返回 ZBADVERSION，重新检查而不是误删
        rc = zoo_delete(m_zhandle, path, stat.version);
        if (rc == ZOK || rc == ZNONODE || rc == ZBADVERSION) {
            continue;
        }
        LOG_ERR("znode delete error! rc: %d (%s), path: %s", rc, zerror(rc), path);
        return false;
    }

    LOG_ERR("znode create error! path: %s, still conflicting after %d attempts", path, kMaxAttempts);
    return false;
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
