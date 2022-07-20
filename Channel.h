#pragma once

#include "noncopyable.h"
#include "Timestamp.h"

#include <functional>
#include <memory>

class EventLoop;

/**
 * 理清楚  EventLoop、Channel、Poller之间的关系   《= Reactor模型上对应 Demultiplex
 * Channel 理解为通道，封装了sockfd和其感兴趣的event，如EPOLLIN、EPOLLOUT事件
 * 还绑定了poller返回的具体事件
 *
 * Eventloop包含channel，Eventloop相当于事件分发器Demultiplex
 * 其包含了poller和channel，channel包含了fd和感兴趣以及最终发生的事件，
 * 这些事件最终要向poller注册，最终发生的事件需要poller向channel通知，channel得到相应fd事件通知以后
 * 调用预置的回调操作就可以了
 *
 */
class Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(Timestamp)>;

    Channel(EventLoop *loop, int fd);
    ~Channel();

    // fd得到poller通知以后，处理事件的，也即调用相应的回调方法
    void handleEvent(Timestamp receiveTime);

    // 设置回调函数对象
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 防止当channel被手动remove掉，channel还在执行回调操作
    void tie(const std::shared_ptr<void> &);

    int fd() const { return fd_; }
    int events() const { return events_; }

    /**
     * channel本身并不能监听事件的发生，只有poller才能监听事件
     * 所以channel对外提供了一个公有接口，在poller监听到事件发生之后通过这个接口设置revents_，
     * 即真正发生的事件
     **/
    void set_revents(int revt)
    {
        revents_ = revt;
    }

    /**
     * 设置fd相应的事件状态
     * channel包含的是fd，跟fd感兴趣的事件，实际上在编程的时候
     * fd对某个事件感兴趣是需要通过epoll_ctl()函数来添加、修改或者删除fd的事件
     * 所以对于channel而言，比如想要使得fd对于读事件感兴趣，则需要对events_变量进行位或操作
     * 然后使用update通知poller调用epoll_ctl()函数，从而把fd感兴趣的事件添加进epoll对象中去
     **/
    void enableReading()
    {
        events_ |= kReadEvent;
        update();
    }
    void disableReading()
    {
        events_ &= ~kReadEvent;
        update();
    }
    void enableWriting()
    {
        events_ |= kWriteEvent;
        update();
    }
    void disableWriting()
    {
        events_ &= ~kWriteEvent;
        update();
    }
    void disableAll()
    {
        events_ = kNoneEvent;
        update();
    }

    // 返回fd当前的事件状态
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }

    /**
     * one loop per thread
     * 我们不可能在多核的系统上用一个线程作为Eventloop，我们肯定会设置跟核数相对应的Eventloop线程
     * 一个线程有一个Eventloop，一个Eventloop里有一个poller，一个poller上可以监听很多个channel
     * 所以每一个channel都是属于某一个Eventloop的，但是一个Eventloop可以对应很多个channel，
     * 这也是被称为多路复用的原因
     **/
    EventLoop *ownerLoop() { return loop_; }
    void remove();

private:
    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    /**
     * 这三个参数表示当前fd的状态，
     * 是不对任何事件感兴趣还是对读事件感兴趣还是对写事件感兴趣
     **/
    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_; // 事件循环
    const int fd_;    // fd, Poller监听的对象
    int events_;      // 注册fd感兴趣的事件
    int revents_;     // poller返回的具体发生的事件
    int index_;

    std::weak_ptr<void> tie_;
    bool tied_;

    /**
     * 因为channel通道里面能够获知fd最终发生的具体的事件revents，所以它负责调用具体事件的回调操作
     * 这些回调函数肯定不是channel决定的，而是用户指定的，通过接口传递给channel，channel来负责调用
     * 因为只有channel才知道fd上发生了什么事件
     */
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
