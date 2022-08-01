#pragma once

/**
 * 在代码上不管是用接口类TcpServer编写服务器还是用TcpClient编写客户端程序，还是在看源码的时候
 * 看到其他相关的类，像包括EventLoop、Acceptor、Channel...每个类都是继承自noncopyable
 * 从名字上来看这个类是不可复制的，其把拷贝构造和赋值函数都给delete掉了、并且有着默认的构造和析构函数
 * TcpServer的对象是能够创建的，因为派生类对象的创建和析构会调用基类的构造函数，而基类的构造函数是被protected
 * 关键词修饰的，是可以被派生类访问的。当对TcpServer对象进行拷贝构造和赋值的时候，
 * 派生类的拷贝构造和赋值肯定要先调用基类的拷贝构造和赋值，然后才是基类部分的拷贝构造和赋值，但是却把基类的拷贝构造和赋值操作给delete掉，
 * 意味着不可以对派生类对象进行拷贝构造和赋值操作
 *
 * 有些人会说没必要搞这么麻烦，可以直接把TcpServer的拷贝构造函数直接delete掉就好了，这种想法当然是可以的，
 * 但是这是一个很大的项目，项目里面有非常多的类，如果要使这么多的类都不能进行拷贝构造和赋值的话，则必须对这些类的拷贝构造和赋值函数都进行一遍delete操作
 * 这种设计还是比较冗余的
 *
 * noncopyable被继承以后，派生类对象可以正常的构造和析构，但是派生类对象
 * 无法进行拷贝构造和赋值操作
 */

class noncopyable {
   public:
    noncopyable(const noncopyable &) = delete;
    noncopyable &operator=(const noncopyable &) = delete;

   protected:
    noncopyable() = default;
    ~noncopyable() = default;
};