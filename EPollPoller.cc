#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <strings.h>

// channel未添加到poller中
const int kNew = -1; // channel的成员index_ = -1
// channel已添加到poller中
const int kAdded = 1;
// channel从poller中删除
const int kDeleted = 2;

EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop),
      // epoll操作的第一步：epoll_create，创建epoll对象
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) // vector<epoll_event>
{
    if (epollfd_ < 0)
    {
        LOG_FATAL("epoll_create error:%d \n", errno);
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

/**
 * EventLoop类中有一个channellist，所有的channel都被eventloop管理
 * channel被创建以后，向poller中注册过的以及未注册过的channel全都被放在list中，
 * 向poller注册过的channel会被写入poller中的channelMap里
 * eventloop会创建一个channellist，并把这个list的地址传给poll，poll函数的作用是通过epoll_wait函数监听到
 * 哪些fd、channel发生了事件，并把真正发生事件的channel通过形参activeChannels告知给eventloop
 *
 **/
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // 实际上应该用LOG_DEBUG输出日志更为合理
    LOG_INFO("func=%s => fd total count:%lu \n", __FUNCTION__, channels_.size());

    /**
     * 第二个参数本身应该存放发生事件fd的event数组，但是实际上为了更方便地扩容
     * 我们装event的数组用的是vector，所以我们需要拿到vector底层数组的起始地址
     *
     * epoll操作的第二步：epoll_wait，阻塞线程让epoll对象监听注册fd感兴趣的事件发生
     **/
    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int saveErrno = errno;
    Timestamp now(Timestamp::now());

    if (numEvents > 0)
    {
        LOG_INFO("%d events happened \n", numEvents);
        fillActiveChannels(numEvents, activeChannels);
        // 监听的所有fd感兴趣的事件都发生了，则需要对events_扩容
        if (numEvents == events_.size())
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG("%s timeout! \n", __FUNCTION__);
    }
    else
    {
        if (saveErrno != EINTR)
        {
            errno = saveErrno;
            LOG_ERROR("EPollPoller::poll() err!");
        }
    }
    return now;
}

/**
 *  channel update remove
 *      调用 EventLoop updateChannel removeChannel
 *            调用 Poller updateChannel removeChannel
 *
 *            EventLoop  =>   poller.poll
 *     ChannelList      Poller
 *                     ChannelMap  <fd, channel*>   epollfd
 */
void EPollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d events=%d index=%d \n", __FUNCTION__, channel->fd(), channel->events(), index);

    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }

        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else // channel已经在poller上注册过了
    {
        int fd = channel->fd();
        // channel对任何事情都不感兴趣了
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

// 从poller中删除channel
void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    channels_.erase(fd);

    LOG_INFO("func=%s => fd=%d\n", __FUNCTION__, fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

/**
 * 填写活跃的连接
 * numEvents：发生事件的个数
 * events_：
 *      using EventList = std::vector<epoll_event>;
 *      EventList events_;
 *
 * activeChannels：实际发生了事件的fd对应的channel集合
 **/
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; ++i)
    {
        // void* -> Channel*
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel); // EventLoop就拿到了它的poller给它返回的所有发生事件的channel列表了
    }
}

// 更新channel通道 epoll_ctl add/mod/del
void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    bzero(&event, sizeof event);

    int fd = channel->fd();

    // typedef union epoll_data
    // {
    //     void *ptr;
    //     int fd;
    //     uint32_t u32;
    //     uint64_t u64;
    // } epoll_data_t;

    // struct epoll_event
    // {
    //     uint32_t events;   /* Epoll events */
    //     epoll_data_t data; /* User data variable */
    // };

    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;

    // epoll操作的第三步：epoll_ctl
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        }
        else
        {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
        }
    }
}