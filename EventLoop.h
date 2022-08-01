#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "CurrentThread.h"
#include "Timestamp.h"
#include "noncopyable.h"

class Channel;
class Poller;

/**
 * 一个Eventloop相当于就是一个reactor，Eventloop类中有成员变量Poller，
 * Poller相当于一个抽象，用Poller当作一个基类，在派生类中实现了poll和epoll这些IO复用的具体分操作，
 * 但是光有epoll是不行的，epoll还得监听socket，所以在Eventloop中还有一个比较重要的成员变量就是channel，
 * 纵观Eventloop类的成员变量，除了编译器内置的类型以外，自定义类型就只有两个：channel和poller，也就是事件分发器中最重要的两个模块，
 * 一个是epoll，一个是epoll感兴趣的事件，一个事件event就绑定了一个socket文件描述符fd以及感兴趣的事件，
 * 所以进入Channel类中观察成员变量就可以看到
    const int fd_;    // fd, Poller监听的对象
    int events_;      // 注册fd感兴趣的事件
    int revents_;     // poller返回的具体发生的事件
 *
 * channel所代表的就是一个fd以及它所绑定的感兴趣的事件和实际上在这个fd上所发生的事件
 * 上述的内容都被封装在了channel类中，所以可以把Eventloop就想象成一个事件分发器，就是一个epoll_wait
 *
 * 简单而言Eventloop就是一个事件循环，事件循环最重要的两个东西：epoll和epoll所监听的fd以及fd所感兴趣的事件以及最终发生事件以后epoll_wait给应用通知的事件
 * socketfd和事件以及感兴趣的事件都在channel里
 *
 * 事件循环类  主要包含了两个大模块 Channel   Poller（epoll的抽象）
 * 一个事件循环包含一个poller，一个poller对应着若干个channel
 * using ChannelList = std::vector<Channel *>;
 *
 *
 **/
class EventLoop : noncopyable {
   public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 开启事件循环
    void loop();
    // 退出事件循环
    void quit();

    Timestamp pollReturnTime() const { return pollReturnTime_; }

    // 在当前loop中执行cb
    void runInLoop(Functor cb);
    // 把cb放入队列中，唤醒loop所在的线程，执行cb
    void queueInLoop(Functor cb);

    // 用来唤醒loop所在的线程的
    void wakeup();

    // EventLoop的方法 =》 Poller的方法
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);

    // 判断EventLoop对象是否在自己的线程里面
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

   private:
    void handleRead();         // wake up
    void doPendingFunctors();  // 执行回调

    using ChannelList = std::vector<Channel *>;

    /**
     * 由一个bool值来控制事件循环是否正常进行，还是退出循环
     * 原子操作，通过CAS实现的
     **/
    std::atomic_bool looping_;
    std::atomic_bool quit_;  // 标识退出loop循环

    /**
     * 记录当前loop所在线程的id
     * 如果当前TcpServer是一个多线程的反应堆模型，则意味着有一个mainReactor和多个subReactor
     * mainReactor相当于就只是监听新用户的链接，通过acceptor拿到新用户的fd，把fd和fd感兴趣的事件打包成一个channel，
     * 然后唤醒某一个subReactor，并把channel传给它，subReactor就代表着一个eventloop，
     * 每一个子线程的eventloop都监听有一组channel，每一组channel发生的事件都得在自己的eventloop线程上去执行相应的回调函数，
     * 这里就是通过threadId来区分每一个eventloop线程
     **/
    const pid_t threadId_;

    Timestamp pollReturnTime_;  // poller返回发生事件的channels的时间点
    std::unique_ptr<Poller> poller_;

    /**
     * mainReactor在给subReactor分配新连接的时候采用的是负载算法是轮询操作，
     * 在没有事件发生的情况下，loop所在的线程都是阻塞的，假如说现在mainReactor监听到了一个新用户的链接，
     * 得到了表示这个新用户链接的fd和其感兴趣事件对应的channel的话，该如何把这个channel传递给subReactor，
     * 因为这些subReactor都处于阻塞状态，所以应该考虑如何把其唤醒
     * 在muduo中使用了linux内核提供的eventfd函数
     *  eventfd()  creates an "eventfd object" that can be used as an event
     *wait/notify mechanism by user-space appli‐ cations, and by the kernel to
     *notify user-space applications of  events.
     * 其中的wait/notify意味着这个函数提供了线程间的通信机制，可以唤醒其他线程
     *
     * 主要作用，当mainLoop获取一个新用户的channel，通过轮询算法选择一个subloop，通过该成员唤醒subloop处理channel
     *
     *
     * 这个wakeupfd的作用更像是信号的作用，在其它一些服务器的实现中一般会使用定时器定时发送信号的方式来结束长时间没被使用到的线程，
     * 这一过程的步骤是：定时器定时触发信号，然后执行响应的信号被触发时的回调函数，在这个回调函数中实现了往管道pipe的写端，也即pipe[1]写一个字节的数据的操作，
     * 这个写操作可以单纯的认为就是用一个标识位来做通知，同时因为linux下的所有东西都被当作是文件，所以这个管道作为一个文件描述符被注册在了epoll对象中，
     * 以便epoll来统一管理事件的发生，这就是所谓的*统一事件源*的概念，也即把信号的处理逻辑也一并当作文件描述符来被epoll来处理
     *
     * 所以我们可以看到涉及到wakeupfd的逻辑就是mainreactor在得到新的连接请求时通知subreactor，这个通知操作就相当于信号的触发
     * 信号回调函数往pipefd中写入数据，而mainreactor往wakeupfd中写入数据，这两者都会导致被epoll所监听的文件描述符感兴趣的事件触发
     * 在这里wakeupfd被封装在了wakeupchannel中
     *
     **/
    int wakeupFd_;
    // wakeupChannel_应该包含wakeupFd_和其感兴趣的事件
    std::unique_ptr<Channel> wakeupChannel_;

    // 一个EventLoop包含一个Poller，一个Poller包含多个Channel，所以一个EventLoop包含多个channel
    ChannelList activeChannels_;

    std::atomic_bool
        callingPendingFunctors_;  // 标识当前loop是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_;  // 存储loop需要执行的所有的回调操作
    std::mutex mutex_;  // 互斥锁，用来保护上面vector容器的线程安全操作
};