#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

#include <functional>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <string>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d TcpConnection Loop is null! \n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop)),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),         // 这里的sockfd就是acceptor接受新的用户连接之后生成的connfd
      channel_(new Channel(loop, sockfd)), // 使用channel封装connfd，TcpConnection的作用就是对包装channel并且传递给subloop
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024) // 一个TcpConnection接收64M数据就到水位线了
{
    /**
     * 下面给channel设置相应的回调函数，poller给channel通知感兴趣的事件发生了，channel会回调相应的操作函数
     *
     * 可以结合Acceptor的对应设置回调函数，Acceptor只关心ReadCallback，只关心拿到新用户连接通信用的fd
     * acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
     * 而已连接用户所感兴趣的事件就比较多了：读、写、出错...
     *
     * 这里的handleRead函数和Acceptor中的handleRead函数的区别在于Acceptor中的handleRead函数是listenfd发生事件对应的回调函数，
     * Acceptor中的handleRead函数主要负责针对listenfd调用accept函数生成connfd
     * 而这里的handleRead函数是connfd发生事件对应的回调函数，并且也是由系统默认设置，执行用户定义的回调操作onMessage
     *
     * 这里需要确认一点，系统默认设置的函数和用户自定义的函数是不冲突的，这个想法基于系统和用户是互斥的逻辑
     * 但是以下的逻辑完全可以成立：系统设置默认函数，这个函数的逻辑是执行用户自定义的函数
     **/
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d \n",
             name_.c_str(), channel_->fd(), (int)state_);
}

/**
 * 用户会给TcpServer注册一个onMessage方法，表示已连接用户有读写事件的时候执行onMessage方法
 * 这个onMessage函数会被TcpServer.setMessageCallback以及TcpConnection.setMessageCallback的形式
 * 注册成messageCallback_,而这个messageCallback_成员函数最终会在TcpConnection中的handleRead回调函数中被调用，
 * 也就是说当connfd发生读事件时会调用handleRead回调函数并且执行逻辑为用户自定义的onMessage函数，而这个onMessage函数中就调用了send函数
 * 我们在onMessage方法中处理完一些业务代码会send给客户端返回数据
 **/
void TcpConnection::send(const std::string &buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));
        }
    }
}

/**
 * 发送数据  应用写的快， 而内核发送数据慢， 需要把待发送数据写入缓冲区， 而且设置了水位回调
 */
void TcpConnection::sendInLoop(const void *data, size_t len)
{
    ssize_t nwrote = 0;
    // 没发送完的数据，刚开始的时候数据都没法送那么remaining就为len
    size_t remaining = len;
    bool faultError = false;

    // 之前调用过该connection的shutdown，不能再进行发送了
    if (state_ == kDisconnected)
    {
        LOG_ERROR("disconnected, give up writing!");
        return;
    }

    /**
     * 刚开始注册的都是socket的读事件，写事件刚开始没注册
     * outputBuffer_.readableBytes() == 0：表示channel_第一次开始写数据，而且写缓冲区没有待发送数据
     *
     *
     * 假设一个这样的场景：
     * 你需要将一个10G大小的文件返回给用户，那么你简单send这个文件是不会成功的。
     * 这个场景下，你send 10G的数据，send返回值不会是10G，而是大约256k，表示你只成功写入了256k的数据。
     * 即len=10G，nwrote=256k，接着调用send，send就会返回EAGAIN，告诉你socket的缓冲区已经满了，此时无法继续send。
     * 此时异步程序的正确处理流程是调用epoll_wait，当socket缓冲区中的数据被对方接收之后，
     * 缓冲区就会有空闲空间可以继续接收数据，此时epoll_wait就会返回这个socket的EPOLLOUT事件，
     * 获得这个事件时，你就可以继续往socket中写出数据。
     *
     * 所以第一次写数据就只写nwrote大小的数据，如果有剩余数据则全部通过触发EPOLLOUT事件，在回调函数hanldeWrite中处理
     * 注意：前面说的缓冲区是sockfd的内核缓冲区，不是我们定义的Buffer缓冲区
     */
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        // 这里的channel_->fd()指的是connfd
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            // remaining==0表示一次性发送完数据，不需要缓冲区暂存
            if (remaining == 0 && writeCompleteCallback_)
            {
                /**
                 * 既然在这里数据全部发送完成，就不用再给channel设置epollout事件
                 * 从而去执行handleWrite回调函数
                 **/
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }
        else // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE  RESET
                {
                    faultError = true;
                }
            }
        }
    }

    // 说明当前这一次write，并没有把数据全部发送出去，剩余的数据需要保存到缓冲区当中，然后给channel
    // 注册epollout事件，LT模式下poller发现tcp的发送缓冲区有空间，会通知相应的sock-channel，调用writeCallback_回调方法
    // 也就是调用TcpConnection::handleWrite方法，把发送缓冲区中的数据全部发送完成
    if (!faultError && remaining > 0)
    {
        // 目前发送缓冲区剩余的待发送数据的长度
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append((char *)data + nwrote, remaining);
        if (!channel_->isWriting())
        {
            /**
             * 这里一定要注册channel的写事件，否则poller不会给channel通知epollout
             * 缓冲区从满到不满，会触发EPOLLOUT事件
             */
            channel_->enableWriting();
        }
    }
}

// 关闭连接
void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明outputBuffer中的数据已经全部发送完成
    {
        // 关闭写端 poller就通知channel出发了关闭事件，就回调TcpConnection的handleclose方法
        socket_->shutdownWrite();
    }
}

// 连接建立
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); // 向poller注册channel的epollin事件

    // 新连接建立，执行回调
    connectionCallback_(shared_from_this());
}

// 连接销毁
void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件，从poller中del掉
        connectionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中删除掉
}

/**
 * TcpConnection封装了一个connfd对应的channel，通过执行channel_->setReadCallback(handleRead)
 * 以便当这个connfd注册的读事件发生之后回调这个handleRead函数
 *
 * 这个handleRead函数的代码隐含着一个先后关系就是只有触发了connfd的读事件才会执行handleRead回调函数，然后才会调用readFd从connfd中读取数据，
 * 但是如果不是先从connfd读取数据又怎么会触发读事件呢？这里如果按照平常的思维就会出现一个先有鸡还是先有蛋的悖论，但是事实并不是这样。
 * 问题的关键在于connfd触发读或者写事件的缘由并不是程序是否调用了read还是write函数，而是取决于套接字缓冲区内是否有数据。
 * 举个例子，客户端往对应的connfd上写数据，那么该connfd的缓冲区内就会存在数据，这并不取决于服务器是否调用了read函数，即使服务器没有调用read函数，
 * connfd的缓冲区内也会存放着从客户端传过来的数据，也即是说当调用read函数的时候，connfd缓冲区就已经
 * 从客户端读取了数据，触发了读事件，而read函数只不过是读取connfd缓冲区内的数据，而并非是调用了read函数之后才从客户端读取数据，
 * write函数同理，并不是说调用了write函数触发了写事件，而是write函数往缓冲区内写数据，缓冲区内有了数据才会触发写事件，如果write往别的没有被注册在epoll的地方
 * 写数据，那么就不会触发写事件。所以事件的触发关键在于缓冲区内数据的变化，而并取决于read或者write函数的调用
 *
 * 缓冲区内有数据，从而导致了read操作
 * write操作导致了缓冲区内有数据
 *
 * 因此对于读事件而言，read操作处于读事件被触发从而调用回调函数handleRead之后
 * 对于写事件而言，write操作处于写事件被触发从而调用回调函数handleWrite之前
 */
void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    // 这里的channel_->fd()为connfd
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    /**
     * 如果从connfd中正常读取数据，则调用messageCallback_回调函数
     * 而这个messageCallback_由用户通过onMessage函数自定义设置，从test_server中的相关代码来看，
     * messageCallback_的逻辑是获取Buffer中可读区域中的数据，并且调用TcpConnection的send方法来处理这些数据
     */
    if (n > 0)
    {
        /**
         * 已建立连接的用户，有可读事件发生了，调用用户传入的回调操作onMessage
         * shared_from_this：获取当前TcpConnection的智能指针
         *
         * messageCallback_为用户自定义的回调函数，不是系统默认设置的，具体案例可以参考test_server.c
         * 下面的一行代码可以等价于onMessage(shared_from_this(), &inputBuffer_, receiveTime),
         * 这里的onMessage是由用户在调用客户端设置的回调函数
         *
         * 还是那个原理，在主程序中调用TcpServer的setMessageCallback函数，设置TcpServer对象的messageCallback_变量，
         * 然后在TcpServer的newConnection函数中调用TcpConnection对象的setMessageCallback函数，
         * 并且以TcpServer对象的messageCallback_为赋值参数，这样就把一个用户定义的回调函数设置到了TcpConnection对象中
         */
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0)
    {
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}

/**
 * 同理handleRead，handleWrite会在confd触发写事件时被调用，主要功能是把在sendInLoop函数中未能一次性发送完的数据全部发送给connfd
 *
 * socket内核缓冲区每次从满到不满都会触发一次EPOLLOUT事件，每次触发EPOLLOUT事件都会调用回调函数handleWrite，
 * 在handleWrite函数中实现了从outputBuffer_用户写缓冲区往channel_->fd()内核缓冲区写数据的操作，然后内核缓冲区内的数据
 * 又会被用户所读取，从而导致缓冲区不满，然后触发EPOLLOUT事件，以此类推，周而复始，直至无数据可写
 */
void TcpConnection::handleWrite()
{
    if (channel_->isWriting())
    {
        int savedErrno = 0;
        // 把发送缓冲区可读区域的数据全部发送到connfd中
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
        if (n > 0)
        {
            /**
             * retrieve的作用就是使readIndex_增大，不断缩小readableBytes，扩大可写入区域大小
             * 因为outputBuffer_.writeFd读取了写缓冲区可读区域中的数据，并且发送到connfd，所以readableBytes变少，即readIndex_增大
             */
            outputBuffer_.retrieve(n);
            /**
             * 发送数据完成
             */
            if (outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting();
                // 与handleRead函数中的messageCallback_赋值原理是相同的
                if (writeCompleteCallback_)
                {
                    // 唤醒loop_对应的thread线程，执行回调
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this()));
                }
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
        LOG_ERROR("TcpConnection fd=%d is down, no more writing \n", channel_->fd());
    }
}

// poller => channel::closeCallback => TcpConnection::handleClose
void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose fd=%d state=%d \n", channel_->fd(), (int)state_);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());
    // 与handleRead函数中的messageCallback_赋值原理是相同的
    connectionCallback_(connPtr); // 执行连接关闭的回调
    closeCallback_(connPtr);      // 关闭连接的回调  执行的是TcpServer::removeConnection回调方法
}

void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d \n", name_.c_str(), err);
}