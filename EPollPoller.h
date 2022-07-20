#pragma once

#include "Poller.h"
#include "Timestamp.h"

#include <vector>
#include <sys/epoll.h>

class Channel;

/**
 * epoll的使用
 * epoll_create =》 EPollPoller(EventLoop *loop);
 * epoll_ctl   add/mod/del  =》 updateChannel、removeChannel
 * epoll_wait   =》 poll
 */
class EPollPoller : public Poller
{
public:
    EPollPoller(EventLoop *loop);
    ~EPollPoller() override;

    // 重写基类Poller的抽象方法
    Timestamp poll(int timeoutMs, ChannelList *activeChannels) override;
    void updateChannel(Channel *channel) override;
    void removeChannel(Channel *channel) override;

private:
    // EventList初始的长度
    static const int kInitEventListSize = 16;

    // 填写活跃的连接
    void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;
    // 更新channel通道
    void update(int operation, Channel *channel);

    /**
     * epoll_wait函数的第二个参数是一个epoll_event的数组，但是数组不好的一点是无法动态扩容
     * 因为平时写的都是一些demo性质的代码所以无所谓扩容不扩容，但是在生产端就必须要考虑这个问题了
     **/
    using EventList = std::vector<epoll_event>;

    // epollfd_通过epoll_create来创建
    int epollfd_;
    EventList events_;
};