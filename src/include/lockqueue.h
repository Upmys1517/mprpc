#pragma once

#include <mutex>
#include <queue>
#include <condition_variable>
#include <thread>

//异步写日志的日志队列
template<class T>
class LockQueue {
public:
    //多个worker线程都会写日志queue
    void push(const T& data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(data);
        m_condition_variable.notify_one();
    }

    //一个线程读日志queue, 写日志文件
    T pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while(m_queue.empty()) {
            //日志队列为空，线程进入wait状态
            m_condition_variable.wait(lock);
        }
        T data = m_queue.front();
        m_queue.pop();
        return data;
    }
private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condition_variable;
};
