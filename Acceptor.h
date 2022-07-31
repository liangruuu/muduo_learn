#pragma once
#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"

#include <functional>

class EventLoop;
class InetAddress;

/**
 * acceptor运行在mainloop中的mainReactor中，其用封装了listenfd的acceptChannel处理accept请求，
 * 如果有新用户的连接响应后就会拿到和客户端通信的connfd，
 * 并且打包成一个channel，根据muduo库默认的轮询算法找到一个subloop，把接受的channel扔给相应的subloop，
 * acceptor又会把connfd和channel打包成一个TcpConnection发送给subloop，在这之前还得唤醒相应的subloop，
 * 也就是说TcpConnection包含了一个connfd和channel，channel包含了一个connfd和event
 *
 * 每一个loop都有一个通过eventfd函数创建的wakeupfd，这个fd也被subloop的poller所监听，mainloop可以通过
 * 给wakeupfd写入一个整数，唤醒响应的loop，然后把新用户连接封装好的channel扔给subloop.
 *
 * mainreactor主要通过acceptor负责新用户的连接事件
 *
 * Acceptor就相当于对listenfd操作的封装
 *
 **/
class Acceptor : noncopyable
{
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;
    // 有了loop才能访问其中的poller，才能把当前acceptor的channel扔给poller，让poller监听channel上面的fd和事件(有无新用户链接)
    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();

    void setNewConnectionCallback(const NewConnectionCallback &cb)
    {
        newConnectionCallback_ = cb;
    }

    bool listenning() const { return listenning_; }
    void listen();

private:
    /**
     * poller监听的listenfd发生感兴趣的事件之后调用的回调函数
     * 虽然handleRead和newConnectionCallback_都是回调函数，但是两者的性质不同，是不同角度的对于用户连接的解读
     * 虽然两者都是因为有用户连接从而导致的函数被执行，从某种意义上来说是有了用户连接，使得有数据被读取，从而导致读事件的发生
     * 前者是因为触发了读事件所以被执行，跟channel是相关联的，因此通过setReadCallback设置在channel中
     * 而后者是因为触发了新用户连接所以被执行，跟channel无关
     */
    void handleRead();

    EventLoop *loop_; // Acceptor用的就是用户定义的那个baseLoop，也称作mainLoop

    /**
     * 把listenfd封装成一个Socket类
     * 本质上也得放在一个poller上，监听listenfd上有没有新用户的连接
     * acceptChannel_打包的fd就是listenfd
     *
     * 关键：在这个系统中每一个可能发生读写事件的都会被封装成一个channel，
     * Acceptor接受用户的新连接，因此会有一个读事件，因此Acceptor接受用户连接的接口listenfd会被封装成channel，
     *
     * 因为服务器开发之后会调用accept函数从而产生connfd，这个connfd是客户端和服务器进行数据传输的接口，
     * 因此connfd也会被封装成channel，只不过这个connfd是在TcpConnection中被定义的，所以TcpConnection类中也会有相关的封装channel的逻辑，
     *
     * 还有就是wakeupfd，因为类似于管道通信的思想，所以也会涉及读写操作，因此也会被封装成channel，因为一个wakeupfd对应一个eventloop，
     * 所以wakeupfd封装成channel的逻辑处于Eventloop类中
     *
     **/
    Socket acceptSocket_;
    Channel acceptChannel_;

    /**
     * 如果Acceptor返回了一个listenfd，也就是说有一个客户端连接成功了
     * Tcpserver接下来就应该通过轮询选择一个subloop唤醒，并把mainloop中获取的connfd打包成channel
     * 扔给subloop，从而让poller去监听已连接fd的读写事件，以上操作就由NewConnectionCallback函数执行
     *
     * 注意这里是NewConnectionCallback，不是TcpConnection中的ConnectionCallback，两者相差了一个New
     **/
    NewConnectionCallback newConnectionCallback_;
    bool listenning_;
};