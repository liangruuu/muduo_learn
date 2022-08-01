#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "Buffer.h"
#include "Callbacks.h"
#include "InetAddress.h"
#include "Timestamp.h"
#include "noncopyable.h"

class Channel;
class EventLoop;
class Socket;

/**
 * TcpConnection主要负责连接，一个连接说的就是服务器和客户端之间建立的一条连接
 * 表示的是连接成功的用户在服务端数据封装的一种表示，mainloop通过acceptor获取新用户的连接以后就会把相应的socket、channel
 * 封装在tcpconnection中，通过轮询算法交给一个subloop，所以TcpConnection对象的成员就包含一个socket和channel，
 * 其实channel对象中还有一个fd，其就是socket底层封装的fd，
 *
 * TcpConnection还拿到了用户预设置的回调函数ConnectionCallback、MessageCallback、WriteCompleteCallback......
 *
 *
 * TcpServer => Acceptor => 有一个新用户连接，通过accept函数拿到connfd
 * =》 打包TcpConnection =》设置回调 Channel =》注册 Poller =》监听到事件发生
 * Channel的回调操作
 *
 */
class TcpConnection : noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {
   public:
    TcpConnection(EventLoop *loop, const std::string &name, int sockfd,
                  const InetAddress &localAddr, const InetAddress &peerAddr);
    ~TcpConnection();

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

    bool connected() const { return state_ == kConnected; }

    // 发送数据
    void send(const std::string &buf);
    // 关闭连接
    void shutdown();

    /**
     * 以下的几种函数最终都会被作为Channel中handleEventWithGuard的callback函数
     *
     * 用户把函数给了TcpServer，TcpServer再把函数给了TcpConnection，TcpConnection再把函数封装到Channel中
     * Channel随后就加入poller，poller监听到事件发生随之channel就调用相应的回调函数
     **/
    void setConnectionCallback(const ConnectionCallback &cb) {
        connectionCallback_ = cb;
    }

    void setMessageCallback(const MessageCallback &cb) {
        messageCallback_ = cb;
    }

    void setWriteCompleteCallback(const WriteCompleteCallback &cb) {
        writeCompleteCallback_ = cb;
    }

    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb,
                                  size_t highWaterMark) {
        highWaterMarkCallback_ = cb;
        highWaterMark_ = highWaterMark;
    }

    void setCloseCallback(const CloseCallback &cb) { closeCallback_ = cb; }

    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

   private:
    // 连接状态
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };
    void setState(StateE state) { state_ = state; }

    // handle函数都会被注册在channel中，等到事件发生时被调用
    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const void *message, size_t len);
    void shutdownInLoop();

    EventLoop *loop_;  // 这里绝对不是baseLoop，
                       // 因为TcpConnection都是在subLoop里面管理的
    const std::string name_;
    std::atomic_int state_;
    bool reading_;

    /**
     * 这里和Acceptor类似   Acceptor=》mainLoop    TcpConenction=》subLoop
     * Acceptor和TcpConenction都需要把底层的fd封装成channel，并且在相应loop的poller中监听事件
     */
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    /**
     * 下面的几个函数都被执行于handleXXX函数中，而handleXXX函数都会被注册在channel中，
     * 最终会在channel对应的fd发生感兴趣的事件时被调用，而这几个函数都是用户所自定义的，详情参考test_server.c
     */
    ConnectionCallback connectionCallback_;  // 有新连接时的回调
    MessageCallback messageCallback_;        // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_;  // 消息发送完成以后的回调
    /**
     * 到达水位回时候地调函数
     * 顾名思义：越过了水位线就会出问题，需要控制在水位线以下
     * 发送数据的时候对端接受数据的速率慢，但是发送地很快，那么数据就会丢失并出错
     * 比如到达水位线了，则可以通过回调函数暂停发送数据
     **/
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;
    size_t highWaterMark_;

    // 接收缓冲区中的read区域数据是从fd中获取的，发送缓冲区中的read区域数据是要往fd中发送的
    Buffer inputBuffer_;   // 接收数据的缓冲区
    Buffer outputBuffer_;  // 发送数据的缓冲区
};