#pragma once

#include "noncopyable.h"
#include "Timestamp.h"

#include <vector>
#include <unordered_map>

class Channel;
class EventLoop;

/**
 * muduo库中多路事件分发器的核心IO复用模块
 * Poller类中全都是纯虚函数，所以Poller本身就是一个抽象类，不能被实例化的
 *
 **/
class Poller : noncopyable
{
public:
    // 对于Poller而言，它要监听很多个channel，所以需要用一个vector来管理
    using ChannelList = std::vector<Channel *>;

    Poller(EventLoop *loop);
    virtual ~Poller() = default;

    /**
     * 给所有IO复用保留统一的接口
     * poll针对EPollPoller来说就相当于epoll_wait，
     * Eventloop中调用统一的poll接口来实现不同的IO复用机制
     *
     * activeChannels：对某些事件感兴趣的channel
     **/
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    // epoll_ctl(add/modify)
    virtual void updateChannel(Channel *channel) = 0;
    // epoll_ctl(delete)
    virtual void removeChannel(Channel *channel) = 0;

    // 判断参数channel是否在当前Poller当中
    bool hasChannel(Channel *channel) const;

    /**
     * EventLoop可以通过该接口获取默认的IO复用的具体实现
     * 具体函数在文件DefaultPoller.c中去实现
     **/
    static Poller *newDefaultPoller(EventLoop *loop);

protected:
    /**
     * map的key：sockfd  value：sockfd所属的channel通道类型
     * Poller所监听的channel是从Eventloop里的channellist来的
     *
     * 对于Poller而言，它要监听很多个channel，所以需要用一个vector来管理，用map只是为了多一个key值对应
     **/
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;

private:
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环EventLoop
};