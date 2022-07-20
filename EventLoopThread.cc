#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr),
      exiting_(false),
      // 调用Thread的构造函数，把EventLoopThread中默认的threadFunc函数作为线程函数
      thread_(std::bind(&EventLoopThread::threadFunc, this), name),
      mutex_(),
      cond_(),
      // threadFunc是线程函数，callback_是线程被创建时的回调函数，两者不是一个概念
      callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit();
        thread_.join();
    }
}

/**
 * thread_.start()函数会启动一个线程，并且执行预先设置的线程函数threadFunc，
 * threadFunc最终会调用EventLoop.loop函数从而开启一个eventloop
 */
EventLoop *EventLoopThread::startLoop()
{
    thread_.start(); // 启动底层的新线程，执行的是func_函数，也就是EventLoopThread::threadFunc

    /**
     * 当我们去执行startLoop的时候，下面的这段代码是一个线程，thread_.start()开启了另一个线程
     * 执行的是threadFunc线程函数，threadFunc线程函数所在线程才是真正去执行eventloop的线程，
     * 所以startLoop需要等待threadFunc线程函数把eventloop初始化好才能继续执行，因此我们可以看到
     * 在threadFunc函数中对loop_进行了赋值操作，等到loop_初始化好了才去通知此线程去使用loop参数
     *
     * 执行startLoop函数就会获取一个新线程，这个新线程中单独运行了一个loop对象，然后可以把这个
     * loop对象返回回去
     **/
    EventLoop *loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr)
        {
            cond_.wait(lock);
        }
        loop = loop_;
    }
    return loop;
}

// 下面这个方法，是在单独的新线程里面运行的
void EventLoopThread::threadFunc()
{
    EventLoop loop; // 创建一个独立的eventloop，和上面的线程是一一对应的，one loop per thread

    // ThreadInitCallback callback_;
    if (callback_)
    {
        callback_(&loop);
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    // 运行loop开启底层Poller的poll方法，而从进入阻塞状态监听远端用户的连接或者已连接用户的读写事件
    loop.loop(); // EventLoop loop  => Poller.poll
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
}