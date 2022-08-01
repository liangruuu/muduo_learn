#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "noncopyable.h"

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : noncopyable {
   public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg);
    ~EventLoopThreadPool();

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    // 开启线程池中的所有线程
    void start(const ThreadInitCallback &cb = ThreadInitCallback());

    // 如果工作在多线程中，baseLoop_默认以轮询的方式分配channel给subloop
    EventLoop *getNextLoop();

    std::vector<EventLoop *> getAllLoops();

    bool started() const { return started_; }
    const std::string name() const { return name_; }

   private:
    /**
     * EventLoop loop;
     * 如果不通过TcpServer的setThreadNumber函数设置底层线程数量的话，那么muduo库就采用的是单线程模型
     * baseloop就是这个单线程模型对应的基础loop，不仅仅作为新用户的连接，还作为已连接用户读写事件的处理loop
     **/
    EventLoop *baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;
    // 通过调用EventLoopThread的startloop函数就能获得EventLoop类型的指针变量，并且存放在loops_数组中
    std::vector<std::unique_ptr<EventLoopThread>> threads_;

    /**
     * 这里其实有一个疑问，为什么已经在有了EventLoopThread对应的threads_的情况下还需要额外定义一个以EventLoop为类型的loops_呢？
     * 其实我们知道EventLoop是以Thread为基础而构建的，EventLoop是以Thread为基础从而构造出了EventLoopThread类型，
     * 也就是说其实暴露给用户使用的是用EventLoop包装而成的Thread，也即EventLoopThread，而并非是单纯的一个Thread
     *
     * 然后用户其实并不想要获取一个线程，而是想要获取线程对应的EventLoop，虽然定义了一个以EventLoopThread为类型的threads_，
     * 但是threads_变量并不提供给用户使用，我们也可以看到EventLoopThreadPool类并没有对外提供threads_变量的接口，反倒是对外提供了loops_
     *
     * threads_单纯的只是一个把Thread封装成一个包含着EventLoop的Thread，类似于装饰器设计模式差不多的概念
     * 所以这里需要设置一个以EventLoop为变量的参数loops_来让用户去获取他们真正想要得到的变量EventLoop
     */
    std::vector<EventLoop *> loops_;
};