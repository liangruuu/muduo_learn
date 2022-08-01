#include "EventLoopThreadPool.h"

#include <memory>

#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop,
                                         const std::string &nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() {}

/**
 * 一个线程池的启动函数做的无非就是执行若干次pthread_create也就是若干个线程的创建，
 * 这个过程在下面也有体现，也就是EventLoopThread的相关操作，可以这么理解EventLoopThread是Thread的装饰类，
 * EventLoopThread在Thread的基础上包装了EventLoop，但是其本质还是一个Thread线程，创建了多个线程之后便把它们放入一个集合vector中
 *
 * 以上是线程池的常规操作，不常规的地方在于该线程池还需要提供给用户获取EventLoop的接口，有多少个Thread就对应多少个EventLoop
 * 因为用户本质来上说并不是在操作Thread，而是在操作EventLoop，EventLoopThread只不过是把两者结合了一下
 * 最终还是要提供一个获取EventLoop的接口，这个接口就是getNextLoop()，只不过getNextLoop()函数跟start启动函数没有什么关系就是了
 *
 * 正如我们在分析std::vector<EventLoop *>
 * loops_时候说的那样，程序需要给用户提供一个获取EventLoop对象的接口，
 * 但前提是EventLoop对象要被提前创建好，不然用户获取的就是一个空的EventLoop对象，因此我们需要在start函数中获取多个已经被初始化
 * 创建好了的EventLoop对象，loops_.push_back(t->startLoop())就是在做这种工作
 *
 * 但这里需要注意的是EventLoopThreadPool的start函数中只是把已经初始化完成的EventLoop对象汇总起来，并不实现EventLoop对象初始化操作
 * 真正完成EventLoop对象初始化操作的地方在EventLoopThread中的startloop函数，如果更进一步的话
 * 其实EventLoopThread中的startloop函数也并不实现EventLoop对象初始化操作，因为之前说过EventLoopThread是基于EventLoop的Thread，
 * 因此EventLoop的初始化操作应该由Thread的线程函数来执行，也就是在EventLoopThread::threadFunc()中被实现
 */
void EventLoopThreadPool::start(const ThreadInitCallback &cb) {
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        /**
         * 可以看到是先创建了EventLoopThread对象，这个EventLoopThread对象是基于Eventloop的线程对象，此时并没有开启eventloop
         * 就把EventLoopThread对象放入vector中，只是做了一些初始化eventloop对象之前的预备工作，例如线程封装类对象Thread的构造，
         * 互斥锁mutex、条件变量cond_variable的预加载，并且在Thread封装类的构造函数中明确了线程函数threadFunc，
         * 在这个线程函数threadFunc中执行用户自定义的线程创建成功的回调函数ThreadInitCallback
         *
         * 直至调用EventLoopThread对象的startloop方法，在startloop方法中执行了new
         * thread(threadFunc)方法创建了一个线程，
         * 并且返回一个初始化成功的loop对象，这个loop对象包含了poller和众多的channel，
         * 并且把初始化成功的loop放入vector中
         */
        EventLoopThread *t = new EventLoopThread(cb, buf);
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
        loops_.push_back(
            t->startLoop());  // 底层创建线程，绑定一个新的EventLoop，并返回该loop的地址
    }

    // 整个服务端只有一个线程，运行着baseloop
    if (numThreads_ == 0 && cb) {
        cb(baseLoop_);
    }
}

/**
 * 如果工作在多线程中，baseLoop_默认以轮询的方式分配channel给subloop
 *
 * 为什么要提供这个接口呢？因为当server接收到http请求之后会将生成的connfd封装成channel发送给各个subLoop
 * 而这个过程中其实就是挑选Loop并且发送的过程，挑选Loop之前则需要获取到Loop，那么getNextLoop就提供了这个功能
 */
EventLoop *EventLoopThreadPool::getNextLoop() {
    EventLoop *loop = baseLoop_;

    /**
     * 通过轮询获取下一个处理事件的loop
     * IO线程运行的是baseloop，工作线程运行的就是新创建的loop，
     * IO线程一般都只处理连接时间，工作线程处理已连接用户的读写事件
     **/
    if (!loops_.empty()) {
        loop = loops_[next_];
        ++next_;
        if (next_ >= loops_.size()) {
            next_ = 0;
        }
    }

    return loop;
}

std::vector<EventLoop *> EventLoopThreadPool::getAllLoops() {
    if (loops_.empty()) {
        return std::vector<EventLoop *>(1, baseLoop_);
    } else {
        return loops_;
    }
}