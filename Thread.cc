#include "Thread.h"

#include <semaphore.h>

#include "CurrentThread.h"

std::atomic_int Thread::numCreated_(0);

/**
 * 任何函数或者变量都区分定义和声明，
 * 定义时不执行代码，只有声明的时候才去执行代码
 */
Thread::Thread(ThreadFunc func, const std::string &name)
    : started_(false),
      joined_(false),
      tid_(0),
      func_(std::move(func)),
      name_(name) {
    setDefaultName();
}

Thread::~Thread() {
    /**
     * 线程已经运行起来了才需要做线程回收的操作
     * thread_->detach()把线程设置为分离线程，也即是说成为了一个守护线程
     * 当主线程结束的时候守护线程会自动结束，内核资源会自动回收
     **/
    if (started_ && !joined_) {
        thread_->detach();  // thread类提供的设置分离线程的方法
    }
}

void Thread::start()  // 一个Thread对象，记录的就是一个新线程的详细信息
{
    started_ = true;
    sem_t sem;
    sem_init(&sem, false, 0);

    // 开启线程
    thread_ = std::shared_ptr<std::thread>(new std::thread([&]() {
        // 获取线程的tid值
        tid_ = CurrentThread::tid();
        // 创建线程成功后会释对信号量进行P操作
        sem_post(&sem);
        // 开启一个新线程，专门执行该线程函数
        func_();
    }));

    // 这里必须等待获取上面新创建的线程的tid值，即必须确保创建线程成功
    sem_wait(&sem);
}

void Thread::join() {
    joined_ = true;
    thread_->join();
}

void Thread::setDefaultName() {
    int num = ++numCreated_;
    if (name_.empty()) {
        char buf[32] = {0};
        snprintf(buf, sizeof buf, "Thread%d", num);
        name_ = buf;
    }
}