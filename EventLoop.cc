#include "EventLoop.h"
#include "Logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>

// 防止一个线程创建多个EventLoop   thread_local
__thread EventLoop *t_loopInThisThread = nullptr;

// 定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000;

// 创建wakeupfd，用来notify唤醒subReactor处理新来的channel
int createEventfd()
{
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
        LOG_FATAL("eventfd error:%d \n", errno);
    }
    return evtfd;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      callingPendingFunctors_(false),
      threadId_(CurrentThread::tid()),
      poller_(Poller::newDefaultPoller(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_))
{
    LOG_DEBUG("EventLoop created %p in thread %d \n", this, threadId_);
    if (t_loopInThisThread)
    {
        LOG_FATAL("Another EventLoop %p exists in this thread %d \n", t_loopInThisThread, threadId_);
    }
    else // 当前线程第一次创建eventloop对象，则给全局变量赋值
    {
        t_loopInThisThread = this;
    }

    /**
     * 构造函数的wakeupChannel_(new Channel(this, wakeupFd_))这一行
     * 只是注册了wakeupFd_并且绑定channel，但是并没有绑定感兴趣的事件，因此poller并不知道需要监听什么事件
     *
     * 所以需要设置wakeupfd的事件类型以及发生事件后的回调操作
     *
     * 其实wakefd所对应的回调函数并不需要做什么，主要的作用还是唤醒阻塞的subreactor对应的eventloop
     * subeventloop阻塞在poller_->poll()这一行，那么mainloop就可以通过wakefd来唤醒subloop，从而继续执行下面的代码逻辑
     *
     * 按照我们对于wakeupfd的分析，因为其作用就是相当于一个信号，那么理所应当在wakeupchannel上注册的回调函数
     * 也应该等同于在信号处理回调函数中的逻辑，我们一般把这个信号处理回调函数命名为sig_handler()，
     * 而在信号处理回调函数sig_handler中我们实现了一个往pipe[1]中send一个标识数据的逻辑，
     * 目的是激活注册在epoll对象中的pipe[0]的读事件，这个统一事件源的概念，即把信号当作文件来处理，以此来触发被epoll所监听的事件。
     * 所以同理我们在wakeupfdchannel上注册的回调函数也应该实现这么一个往对应文件描述符上写一个通知标识数据的逻辑，
     * 那么这个handleRead函数就执行了这么一个功能
     **/
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    /**
     * 读事件发生以后就去执行handleRead回调函数
     * 每一个eventloop都将监听wakeupchannel的EPOLLIN读事件了
     * mainloop就可以通过给wakeupfd写东西来通知subreactor起来做事情了
     **/
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

// 开启事件循环
void EventLoop::loop()
{
    looping_ = true;
    quit_ = false;

    LOG_INFO("EventLoop %p start looping \n", this);

    while (!quit_)
    {
        activeChannels_.clear();
        /**
         * 监听两类fd   一种是client的fd，一种wakeupfd
         * clientfd就是正常和客户端通信的socket链接，wakeupfd是mainloop和subloop之间通信的eventfd
         * subLoop阻塞在这一行代码上，mainLoop通过wakeupFd_唤醒阻塞的subLoop
         **/
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        for (Channel *channel : activeChannels_)
        {
            // Poller监听哪些channel发生事件了，然后上报给EventLoop，通知channel处理相应的事件
            channel->handleEvent(pollReturnTime_);
        }

        /**
         * 执行当前EventLoop事件循环需要处理的回调操作，channel有事件回调函数，EventLoop也有回调函数
         *
         * IO线程 mainLoop accept fd(channel) =》 subloop
         *
         * 因为mainLoop只做新用户的链接，已连接用户的channel需要分发给subLoop
         * 如果说没有调用过muduo库的setThreadNumber函数的话，也就是说只有一个mainLoop，mainLoop不仅仅只做新用户的连接操作，
         * 也负责已连接用户的读写事件，因为现在的计算机多是多核CPU，所以我们会去启动多个subLoop，当mainLoop接受到一个新用户链接的时候，
         * 就会唤醒一个subLoop
         * mainLoop 事先注册一个回调cb（需要subloop来执行），但是此subloop还在被poller_->poll语句阻塞
         * 通过eventfd wakeup subloop后，执行下面的方法，执行之前mainloop注册的cb操作，有可能是一个cb也有可能是多个cb
         *
         * 之所以被设置回调操作，肯定是因为被派发了新的channel了，让这个subloop去处理这个channel，最起码这个loop需要执行
         * 把channel添加到channel列表、把channel去往subloop注册等操作，这些操作就是由回调函数去驱动的
         */
        doPendingFunctors();
    }

    LOG_INFO("EventLoop %p stop looping. \n", this);
    looping_ = false;
}

/**
 * 退出事件循环  1.loop在自己的线程中调用quit  2.在非loop的线程中，调用loop的quit
 *
 *              mainLoop
 *
 *                                       no ==================== 生产者-消费者的线程安全的队列
 *
 *  subLoop1     subLoop2     subLoop3
 */
void EventLoop::quit()
{
    quit_ = true;

    // 如果是在其它线程中，调用的quit   在一个subloop(woker)中，调用了mainLoop(IO)的quit
    if (!isInLoopThread())
    {
        wakeup();
    }
}

// 在当前loop中执行cb
void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread()) // 在当前的loop线程中，执行cb
    {
        cb();
    }
    else // 在非当前loop线程中执行cb , 就需要唤醒loop所在线程，执行cb
    {
        queueInLoop(cb);
    }
}
// 把cb放入队列中，唤醒loop所在的线程，执行cb
void EventLoop::queueInLoop(Functor cb)
{
    // 因为pendingFunctors_可能被多个线程访问，所以需要设置锁
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb);
    }

    // 唤醒相应的，需要执行上面回调操作的loop的线程了
    // || callingPendingFunctors_的意思是：当前loop正在执行回调，但是loop又有了新的回调
    if (!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup(); // 唤醒loop所在线程
    }
}

/**
 * handleRead具体是什么逻辑不重要，重要的是每一个subreactor都监听了一个wakeupchannel
 * mainreactor就可以通过给wakeupchannel发送(write)一个消息，则subreactor就可以感知到wakeupfd上有读事件发生，
 * 并且由此被唤醒，从而获取到mainloop传递过来的新用户链接的channel
 **/
void EventLoop::handleRead()
{
    // 8字节
    uint64_t one = 1;
    /**
     * 等价于从信号操作中从pipefd[0]中读取一个字节的数据，因为pipefd[0]的读事件被注册在了epoll对象中,
     * 而read函数又读取了从pipefd[1]中写入的一字节标识数据，从而触发了对应被注册在了epoll对象中的EPOLLIN事件
     */
    ssize_t n = read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::handleRead() reads %lu bytes instead of 8", n);
    }
}

// 用来唤醒loop所在的线程的  向wakeupfd_写一个数据，wakeupChannel就发生读事件，当前loop线程就会被唤醒
void EventLoop::wakeup()
{
    uint64_t one = 1;
    /**
     * 我们在EventLoop.h文件中已经详细地分析了wakeupfd的作用，对wakeupfd的操作跟对信号操作是一个性质
     * 所以这里对wakeupfd写一个字节的数据等价于在信号回调函数中往pipefd[1]写入一个标识数据
     */
    ssize_t n = write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::wakeup() writes %lu bytes instead of 8 \n", n);
    }
}

// EventLoop的方法 =》 Poller的方法
void EventLoop::updateChannel(Channel *channel)
{
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel *channel)
{
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel *channel)
{
    return poller_->hasChannel(channel);
}

void EventLoop::doPendingFunctors() // 执行回调
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const Functor &functor : functors)
    {
        functor(); // 执行当前loop需要执行的回调操作
    }

    callingPendingFunctors_ = false;
}