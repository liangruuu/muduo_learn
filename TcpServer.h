#pragma once

/**
 * 用户使用muduo编写服务器程序
 */
#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

/**
 * 对外的服务器编程使用的类
 * TcpServer相当于muduo库给外界提供编写服务器程序的一个入口类，相当于一个大箱子，把muduo库中跟服务器编程相关的
 * 比如反应堆、事件分发器、事件回调都给打包到一块了，其实我们也能从它的成员函数和成员变量中看到，比如EventLoop、
 * Acceptor、EventLoopThreadPool（事件分发器相当于Epoll，不可能只是对应着一个线程，在多核的CPU下，一个线程去做epoll_wait，去做事件分发
 * 有点浪费资源，所以其肯定有一个事件循环的线程池）、一系列回调函数、ConnectionMap（维护了所有客户端连接）
 *
 * 而EventLoop是Reactor模型中最重要的一个部分，也就是对应着事件分发器模块
 *
 **/
class TcpServer : noncopyable
{
public:
    /**
     * 启动一个线程时候的回调函数
     * 这个回调函数会在EventLoopThread类中的threadFunc函数中被调用
     *  if (callback_)
        {
            callback_(&loop);
        }
     * 因为这个callback_有一个loop参数，所以ThreadInitCallback也是一个以EventLoop为形参的函数
     *
     * TcpServer是其他所有muduo组件的"中介站"
     * 类似于ThreadInitCallback、ConnectionCallback、MessageCallback...等回调函数的主要逻辑处理地方不在TcpServer，
     * 而是在TcpConnection、EventLoopThreadPool等地
     *
     * 比如ThreadInitCallback线程初始化回调函数将会直接由EventLoopThread操作
     * 再比如MessageCallback读写消息回调函数将会直接由TcpConnection操作
     * TcpServer只是起到了一个把客户设置的回调函数间接地传给了这些直接操作相关回调函数组件的功能
     * 即客户端->(设置函数)->TcpServer->(设置函数)->相关组件
     *
     * 因此除了在特定的组件内定义相关函数变量，也必须要在TcpServer内定义相应的函数变量用于承接来自客户端的传参
     * 并且把这些函数参数再次设置给其余组件
     *
     * ThreadInitCallback
     * :客户端->TcpServer->EventLoopThreadPool->EventLoopThread
     *
     * ConnectionCallback
     * :客户端->TcpServer->TcpConnection
     *
     * MessageCallback
     * :客户端->TcpServer->TcpConnection
     *
     * WriteCompleteCallback
     * :客户端->TcpServer->TcpConnection
     **/
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    enum Option
    {
        kNoReusePort,
        kReusePort,
    };

    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    /**
     * 下面的四个函数跟TcpConnection中的函数是一模一样的
     * 用户把函数给了TcpServer，TcpServer再把函数给了TcpConnection，TcpConnection再把函数封装到Channel中
     * 因为有这一层递进的关系，所以在test_server.c中是调用了TcpServer的set回调函数，
     * 以setConnectionCallback函数为例，在test_server.c中是调用了TcpServer的set回调函数设置了TcpServer对象的connectionCallback_变量，
     * 然后在TcpServer中又把TcpServer对象的connectionCallback_变量作为参数调用了TcpConnection的set回调函数
     * conn->setConnectionCallback(connectionCallback_)的格式，经过这一连串的过程最终设置了TcpConnection对象的响应回调函数
     *
     * 因为在TcpConnection中最终会设置handleXXX函数，这个handleXXX函数才是最终在channel中因为触发事件所以被调用的回调函数，
     * 所以我们需要经过这么一长串过程，最终设置TcpConnection中的回调函数，而并非是止步于TcpServer中的回调函数
     */
    void setThreadInitcallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

    // 设置底层subloop的个数
    void setThreadNum(int numThreads);

    // 开启服务器监听
    void start();

private:
    // 封装TcpConnection对象
    void newConnection(int sockfd, const InetAddress &peerAddr);
    void removeConnection(const TcpConnectionPtr &conn);
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    /**
     * TcpServer作为用户和服务器的直接桥梁，拥有很多重要的参数
     * 1. EventLoop *loop_: 根据one loop per thread的思想，TcpServer运行在main线程上
     * 因此需要有一个配套的mainLoop来运行循环，这个mainLoop的主要作用就是listen来自客户端的连接请求
     * 2. Acceptor acceptor_: 接收来自客户端的连接请求的组件
     * 3. ConnectionCallback、MessageCallback等多个回调函数
     * 4. TcpConnection: 每个来自客户端的请求都会在TcpServer中被封装成TcpConnection从而发送给subLoop进行处理
     * 5. EventLoopThreadPool threadPool_: TcpServer操作线程池去创建线程以便运行多个subLoop
     */
    EventLoop *loop_; // baseLoop 用户定义的loop

    const std::string ipPort_;
    const std::string name_;

    std::unique_ptr<Acceptor> acceptor_; // 运行在mainLoop，任务就是监听新连接事件

    std::shared_ptr<EventLoopThreadPool> threadPool_; // one loop per thread

    // 以下三个函数会被底层reactor组件调用
    ConnectionCallback connectionCallback_;       // 有新连接时的回调
    MessageCallback messageCallback_;             // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成以后的回调

    ThreadInitCallback threadInitCallback_; // loop线程初始化的回调

    std::atomic_int started_;

    int nextConnId_;
    ConnectionMap connections_; // 保存所有的连接
};