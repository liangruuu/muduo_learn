#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

#include <memory>

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
}

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        /**
         * 可以看到是先创建了EventLoopThread对象，这个EventLoopThread对象是基于Eventloop的线程对象，此时并没有开启eventloop
         * 就把EventLoopThread对象放入vector中，只是做了一些初始化eventloop对象之前的预备工作，例如线程封装类对象Thread的构造，
         * 互斥锁mutex、条件变量cond_variable的预加载，并且在Thread封装类的构造函数中明确了线程函数threadFunc
         *
         * 直至调用EventLoopThread对象的startloop方法，在startloop方法中执行了thread(threadFunc)方法创建了一个线程，
         * 并且返回一个初始化成功的loop对象，这个loop对象包含了poller和众多的channel，
         * 并且把初始化成功的loop放入vector中，一个thread
         */
        EventLoopThread *t = new EventLoopThread(cb, buf);
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
        loops_.push_back(t->startLoop()); // 底层创建线程，绑定一个新的EventLoop，并返回该loop的地址
    }

    // 整个服务端只有一个线程，运行着baseloop
    if (numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

// 如果工作在多线程中，baseLoop_默认以轮询的方式分配channel给subloop
EventLoop *EventLoopThreadPool::getNextLoop()
{
    EventLoop *loop = baseLoop_;

    /**
     * 通过轮询获取下一个处理事件的loop
     * IO线程运行的是baseloop，工作线程运行的就是新创建的loop，
     * IO线程一般都只处理连接时间，工作线程处理已连接用户的读写事件
     **/
    if (!loops_.empty())
    {
        loop = loops_[next_];
        ++next_;
        if (next_ >= loops_.size())
        {
            next_ = 0;
        }
    }

    return loop;
}

std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return std::vector<EventLoop *>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}