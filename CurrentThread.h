#pragma once

#include <unistd.h>
#include <sys/syscall.h>

/**
 * TcpServer提供了一个setThreadNumber函数，也就是说服务器可以提供多个EventLoop，
 * 每一个EventLoop对应一个线程，对应着多个channel，自己channel上发生的事件要在自己的eventloop线程上处理，
 * 所以CurrentThread中提供了获取当前线程ID的方法
 **/
namespace CurrentThread
{
    extern __thread int t_cachedTid;

    void cacheTid();

    inline int tid()
    {
        if (__builtin_expect(t_cachedTid == 0, 0))
        {
            cacheTid();
        }
        return t_cachedTid;
    }
}