#pragma once

#include "noncopyable.h"

#include <functional>
#include <thread>
#include <memory>
#include <unistd.h>
#include <string>
#include <atomic>

/**
 * EventLoop包括其底层的Poller不会只工作在一个线程上，其是one loop per thread：一个线程对应一个loop，
 **/
class Thread : noncopyable
{
public:
    /**
     * 线程函数
     * 这里的线程函数是默认的无参函数，如果想要设置有参函数，则可以使用绑定器bind构造成一个无参函数形式
     **/
    using ThreadFunc = std::function<void()>;

    explicit Thread(ThreadFunc, const std::string &name = std::string());
    ~Thread();

    void start();
    void join();

    bool started() const { return started_; }
    pid_t tid() const { return tid_; }
    const std::string &name() const { return name_; }

    static int numCreated() { return numCreated_; }

private:
    void setDefaultName();

    bool started_;
    bool joined_;
    std::shared_ptr<std::thread> thread_;
    pid_t tid_;
    ThreadFunc func_;
    std::string name_;
    static std::atomic_int numCreated_;
};