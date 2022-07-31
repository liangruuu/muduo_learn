#include "TcpServer.h"
#include "Logger.h"
#include "TcpConnection.h"

#include <strings.h>
#include <functional>

// 不允许用户给loop传递一个空指针，如果这样的话就意味着连mainloop都没有了
static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null! \n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

// 这里的loop指的是mainloop，也就是设置了acceptor的loop
TcpServer::TcpServer(EventLoop *loop,
                     const InetAddress &listenAddr,
                     const std::string &nameArg,
                     Option option)
    : loop_(CheckLoopNotNull(loop)),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      // acceptor_处于mainloop线程中，监听listenfd，在这个构造函数中已经完成了第一步socket和第二步bind操作
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new EventLoopThreadPool(loop, name_)),
      connectionCallback_(),
      messageCallback_(),
      nextConnId_(1),
      started_(0)
{
    /**
     * 当有先用户连接时，会执行TcpServer::newConnection回调
     * 注意这里是setNewConnectionCallback不是TcpConnection中的setConnectionCallback，两者相差了一个New，
     * 这个New也可以看出acceptor处理的是客户端第一次和服务器的网络连接，处理的是两端的链路连接而非数据的传输
     * 并且setNewConnectionCallback处理的回调函数是由系统设置的，而TcpConnection中的setConnectionCallback中的ConnectionCallback
     * 是由用户指定的，可以理解为NewConnectionCallback是用户连接建立时候系统设置的缺省回调函数，是必须要执行的，
     * 而TcpConnection中的ConnectionCallback设置的选择权在用户手中，设置与不设置都是可以的
     */
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                                  std::placeholders::_1,
                                                  std::placeholders::_2));
}

TcpServer::~TcpServer()
{
    for (auto &item : connections_)
    {
        // 这个局部的shared_ptr智能指针对象，出右括号，可以自动释放new出来的TcpConnection对象资源了
        TcpConnectionPtr conn(item.second);
        item.second.reset();

        /**
         * 销毁连接
         *
         * 因为conn->getLoop()获得的是TcpConnection的Loop，因此这里的loop对应的线程threadId_为sub线程
         * 而~TcpServer函数的调用方TcpServer处于main线程中，因此runInLoop函数的isInLoopThread判断为false
         * 因此需要把connectDestroyed函数存入pendingFunctors队列中来让ioLoop去执行
         *
         * 其实也是比较好理解的，针对每个不同的TcpConnection都需要在对应的loop线程中去执行销毁conn的操作
         */
        conn->getLoop()->runInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
    }
}

// 设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    threadPool_->setThreadNum(numThreads);
}

/**
 * 开启服务器监听
 *
 * TcpServer的start函数会调用EventLoopThreadPool的start函数，同时传入一个线程被创建成功时候的回调函数threadInitCallback_，
 * 这个回调函数将在线程函数threadFunc中被调用，threadPool_的start函数主要是创建EventLoopThread对象，
 * 同时把回调函数threadInitCallback_作为EventLoopThread构造函数参数，而在EventLoopThread对象中定义了Thread变量，
 * 这个Thread变量封装了真正起到线程作用的thread_，这个thread_是一个linux中的thread变量，并会以thread(threadFunc)的形式创建一个线程，
 *
 * EventLoopThreadPool中定义了threads_和loops_变量，前者是线程的集合，后者是eventloop的集合，
 * 因为threadPool_是一个基于eventloop的线程池，所以必须有这两个变量，所以在start方法中执行了
 * 1. threads_.push_back(std::unique_ptr<EventLoopThread>(t));
 *  这个操作就是把刚创建的EventLoopThread对象放入集合中
 * 2. loops_.push_back(t->startLoop())
 *  t->startLoop()实现了两个主要逻辑：
 *  1. 调用了thread_.start()方法，这个start方法中实现了真正开启一个线程的逻辑：thread(threadFunc)，
 *  2. 在线程函数threadFunc中执行了loop.loop()函数，正式开启了eventloop循环，
 *     并且返回了一个完成了初始化的loop对象
 *  因此被取名为startLoop
 *
 * EventLoopThread最关键的两个参数就是eventloop和thread，都在EventLoopThreadPool中有相应的体现
 */
void TcpServer::start()
{
    if (started_++ == 0) // 防止一个TcpServer对象被start多次
    {
        /**
         * threadInitCallback_为线程被创建时的回调函数，并非是线程函数，两者不是一个概念
         * 其跟ConnectionCallback、MessageCallback、WriteCompleteCallback同样都是由用户自定义的
         */
        threadPool_->start(threadInitCallback_); // 启动底层的loop线程池
        /**
         * listen&enableReading，把acceptChannel注册到mainloop的poller上
         *
         * 这个loop_是主线程的对象，所以在runInLoop函数中直接执行对应的回调函数Acceptor::listen
         * 执行listen回调函数就是把用acceptchannel封装的listenfd注册到poller上
         * 调用完start函数，就调用loop的loop()方法开启事件循环，即poller开始运行监听acceptchannel上有没有新用户的连接
         *
         * 因为在TcpServer的构造函数中就初始化了acceptor_对象，因此完成了第1，2步骤
         * 所以这里就可以着手第三步的listen操作
         * 因为是listen操作，所以需要一个线程不断循环，所以这个listen函数被安排在一个Loop线程中执行
         *
         * 这里的loop_是TcpServer成员变量，而TcpServer对象又是在main线程中定义的，因此该loops_是属于main线程的
         * 又因为该runInLoop函数处于start函数中，而调用start函数的地方在testserver.c中，即main线程中调用
         * 所以isInLoopThread()为true，即直接执行cb()
         **/
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

// 有一个新的客户端的连接，acceptor会执行这个回调操作
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
    /**
     * 轮询算法，选择一个subLoop，来管理channel
     * 如果没有设置setThreadNumber，则返回的就是baseloop
     **/
    EventLoop *ioLoop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection [%s] - new connection [%s] from %s \n",
             name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());

    // 通过sockfd获取其绑定的本机的ip地址和端口信息
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrlen = sizeof local;
    if (::getsockname(sockfd, (sockaddr *)&local, &addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr");
    }
    InetAddress localAddr(local);

    // 根据连接成功的sockfd，创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(
        ioLoop,
        connName,
        sockfd, // Socket Channel
        localAddr,
        peerAddr));
    connections_[connName] = conn;
    /**
     * 下面的回调都是用户设置给TcpServer=>设置给TcpConnection=>设置给Channel=>注册Poller=>notify channel调用回调
     *
     * 这里的setConnectionCallback跟setNewConnectionCallback是不同的，这里的回调函数connectionCallback_完全是由用户所指定的，
     * 可以看到这里的connectionCallback_完全是由用户通过TcpConnection提供的接口自定义设置的，
     * 系统没有提供类似于上面的newConnection函数来充当默认回调函数，具体运用案例可以参考testserver.c
     */
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    /**
     * 设置了如何关闭连接的回调   conn->shutDown()
     * 可以看到这里的setCloseCallback提供了一个系统默认提供的removeConnection，类似于上面的newConnectionn回调函数
     */
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    /**
     * 直接调用TcpConnection::connectEstablished
     *
     * ioLoop对象的threadId是对应subThread的ID
     * 但是当前的线程是main线程，所以当前所执行的回调并不在loop所在线程
     * 随机执行queueInloop函数，通过往对应sub线程写一个数据从而唤醒该线程，
     * 并且把TcpConnection注册到subloop中
     *
     * runInLoop函数中isInLoopThread()判断的代码=>threadId_ == CurrentThread::tid();
     * 因为isInLoopThread()是EventLoop对应的成员函数，所以threadId_指的是EventLoop对应的threadId_
     * 也就是ioLoop对应的threadId_，而ioLoop又是所谓的subLoop
     * 而CurrentThread::tid()指的是当前调用这个runInLoop函数的线程, 即TcpServer所在的线程
     * 这里有一个技巧，判断当前线程的方法是循环判断函数的调用方
     * 即CurrentThread::tid()的调用方是isInLoopThread()
     * 而isInLoopThread()函数的调用方是runInLoop()函数
     * 而runInLoop()函数的调用方，注意不是ioLoop，虽然是以ioLoop->runInLoop的形式呈现
     * 但是runInLoop()函数的调用方是TcpServer::newConnection()，newConnection调用了ioLoop对象里的runInLoop函数
     * 又因为TcpServer在testserver.cc中的main函数中以子类形式被调用，因此TcpServer所在的线程为main线程
     *
     * 因此由于mainLoop不等于subLoop，于是该connectEstablished函数将会被放入该ioLoop的pendingFunctors_集合中
     * 这里需要注意的是pendingFunctors_是某个EventLoop对象特有的，不同的mainLoop或者subLoop所拥有的pendingFunctors_是不同的
     * pendingFunctors_集合最终会在该ioLoop的loop循环体中被doPendingFunctors函数所调用
     **/
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    /**
     * loop_对象在TcpServer中被定义，而TcpServer又在testserver.c中被声明，即该loop_是baseLoop，对应的线程为main线程
     * 又因为removeConnection在TcpServer对象中被声明，因此CurThread为TcpServer对象所在线程，即main线程
     * 所以threadId_=CurThread，因此isInLoopThread为true，调用runInLoop直接执行cb()，并不需要存入pendingFunctors队列中
     */
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop [%s] - connection %s\n",
             name_.c_str(), conn->name().c_str());

    connections_.erase(conn->name());
    EventLoop *ioLoop = conn->getLoop();

    /**
     * 因为conn->getLoop()获得的是TcpConnection的Loop，因此这里的ioLoop对应的线程threadId_为sub线程
     * 而removeConnectionInLoop函数的调用方TcpServer处于main线程中
     * 因此需要把connectDestroyed函数存入pendingFunctors队列中来让ioLoop去执行
     */
    ioLoop->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
}