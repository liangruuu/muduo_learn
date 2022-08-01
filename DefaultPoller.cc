#include <stdlib.h>

#include "EPollPoller.h"
#include "Poller.h"

Poller *Poller::newDefaultPoller(EventLoop *loop) {
    // 环境变量中设置MUDUO_USE_POLL变量
    if (::getenv("MUDUO_USE_POLL")) {
        return nullptr;  // 生成poll的实例
    } else {
        return new EPollPoller(loop);  // 生成epoll的实例
    }
}