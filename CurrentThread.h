#pragma once

#include <sys/syscall.h>
#include <unistd.h>

/**
 * TcpServer提供了一个setThreadNumber函数，也就是说服务器可以提供多个EventLoop，
 * 每一个EventLoop对应一个线程，对应着多个channel，自己channel上发生的事件要在自己的eventloop线程上处理，
 * 所以CurrentThread中提供了获取当前线程ID的方法
 *
 * 什么是CurrentThread(当前线程)?
 * 我们知道，在一个单核CPU中，虽然应用可以同时创建多个线程
 * 但是事实上在任一时刻，只有一个线程在运行，我们当前在运行的线程称之为当前线程(CurrentThread)
 * 需要注意的是，当前线程是不断的在变化的，因为CPU会一会执行这个线程，一会又去执行另外一个线程
 * 因此当前线程并不是固定的
 *
 **/
namespace CurrentThread {
extern __thread int t_cachedTid;

void cacheTid();

inline int tid() {
    if (__builtin_expect(t_cachedTid == 0, 0)) {
        cacheTid();
    }
    return t_cachedTid;
}
}  // namespace CurrentThread