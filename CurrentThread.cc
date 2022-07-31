#include "CurrentThread.h"

namespace CurrentThread
{
    __thread int t_cachedTid = 0;

    void cacheTid()
    {
        if (t_cachedTid == 0)
        {
            /**
             * syscall(SYS_gettid): 通过linux系统调用，获取当前线程的tid值
             * 有时候我们可能需要知道线程的真实pid
             * 比如进程P1要向另外一个进程P2中的某个线程发送信号时，既不能使用P2的pid，更不能使用线程的pthread id，而只能使用该线程的真实pid，称为tid
             * 有一个函数gettid()可以得到tid，但glibc并没有实现该函数，只能通过Linux的系统调用syscall来获取。
             */
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}