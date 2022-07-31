#include "Acceptor.h"
#include "Logger.h"
#include "InetAddress.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

static int createNonblocking()
{
    // 第一步socket生成listenfd
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
    {
        LOG_FATAL("%s:%s:%d listen socket create err:%d \n", __FILE__, __FUNCTION__, __LINE__, errno);
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop),
      acceptSocket_(createNonblocking()),
      // 只要是可能发生事件的东西都会被封装成channel，Acceptor会发生读事件，因为网络请求就是读写的过程，所以被封转成channel
      acceptChannel_(loop, acceptSocket_.fd()),
      listenning_(false)
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(true);
    // 第二步bind绑定套接字与Socket对象
    acceptSocket_.bindAddress(listenAddr);
    /**
     * TcpServer::start() =》启动 Acceptor.listen()
     * 有新用户的连接，要执行一个回调负责（connfd=》打包 channel=》唤醒 subloop）
     *
     * baseLoop =>poller监听到 acceptChannel_(listenfd)有事件发生=>acceptChannel调用相应的callback函数
     * callback函数事先已经设置为Acceptor::handleRead
     **/
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();
    acceptChannel_.remove();
}

void Acceptor::listen()
{
    listenning_ = true;
    // socket、bind、listen、accept的第三步
    acceptSocket_.listen();
    /**
     * acceptChannel_ =>注册listenfd到Poller，以便发生读事件时调用handleRead回调函数，
     * 而这个handleRead的逻辑是第四步的accept产生connfd，执行newConnectionCallback_函数，
     * 这个newConnectionCallback_在TcpServer类中被设置为newConnection函数
     */
    acceptChannel_.enableReading();
}

// listenfd有事件发生了，就是有新用户连接了
void Acceptor::handleRead()
{
    InetAddress peerAddr;
    // 完成第四步也就是最后一个步骤accept产生connfd，只不过这是在listenfd发生了读事件时在回调函数handleRead中实现的
    int connfd = acceptSocket_.accept(&peerAddr);
    if (connfd >= 0)
    {
        if (newConnectionCallback_)
        {
            /**
             * 这里的newConnectionCallback_(connfd, peerAddr)就相当于newConnection(connfd, peerAddr)
             * newConnection(connfd, peerAddr)是在TcpServer中定义并且通过acceptor设置的
             * 具体为在TcpServer的构造函数中调用acceptor_->setNewConnectionCallback(newConnection)
             */
            newConnectionCallback_(connfd, peerAddr); // 轮询找到subLoop，唤醒，分发当前的新客户端的Channel
        }
        else
        {
            // 如果没有预先设置回调函数，则表示客户端无法处理新用户链接
            ::close(connfd);
        }
    }
    else
    {
        LOG_ERROR("%s:%s:%d accept err:%d \n", __FILE__, __FUNCTION__, __LINE__, errno);
        if (errno == EMFILE)
        {
            LOG_ERROR("%s:%s:%d sockfd reached limit! \n", __FILE__, __FUNCTION__, __LINE__);
        }
    }
}