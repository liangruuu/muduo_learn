#pragma once

#include <vector>
#include <string>
#include <algorithm>

/**
 * 网络库底层的缓冲器类型定义
 * 基于非阻塞IO的服务端编程中，一个缓冲区还是非常有必要的
 * 而且在TCP编程过程中出现的粘包问题，一般都会在通信的数据中加上一个数据头用来描述数据的长度，
 * 每次根据数据的长度来截取数据包，从而进行反序列化处理，可能某一次接受的数据比较多，但是应用只需要读取部分的数据，
 * 所以未读的数据就必须存放在缓冲区中，发送数据也是同样的道理，需要发送的数据可能一次发送不完，则需要把其余未发送的数据暂存在缓冲区中，
 *
 *  +-------------------+------------------+------------------+
 *  | prependable bytes |  readable bytes  |  writable bytes  |
 *  |                   |     (CONTENT)    |                  |
 *  +-------------------+------------------+------------------+
 *  |                   |                  |                  |
 *  |0      <=      readerIndex   <=   writerIndex    <=     size
 *
 * buffer分为三部分，prependable bytes是一个8字节的长度，主要用来解决粘包问题，可以在头8个字节存放数据包的长度，
 * 如果要往缓冲区中写数据就必须从writerIndex开始写，应用程序要读的话就必须从readerIndex指向的地方开始读，
 * 所以Buffer类有三个成员变量就是vector、readIndex(数据可读位置下标)、writeIndex(数据可写位置下标)
 **/

class Buffer
{
public:
    // 记录数据包的长度
    static const size_t kCheapPrepend = 8;
    // 缓冲区初始大小
    static const size_t kInitialSize = 1024;

    // readerIndex_和writerIndex_一开始都指向缓冲区kCheapPrepend大小的位置，因为没有数据
    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize), readerIndex_(kCheapPrepend), writerIndex_(kCheapPrepend)
    {
    }

    // 缓冲区内可读的数据长度
    size_t readableBytes() const
    {
        return writerIndex_ - readerIndex_;
    }

    // 缓冲区内可写数据长度
    size_t writableBytes() const
    {
        return buffer_.size() - writerIndex_;
    }

    size_t prependableBytes() const
    {
        return readerIndex_;
    }

    // 返回缓冲区中可读数据的起始地址
    const char *peek() const
    {
        return begin() + readerIndex_;
    }

    // onMessage string <- Buffer
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            // 应用只读取了刻度缓冲区数据的一部分，就是len，还剩下readerIndex_ += len和writerIndex_之间的数据没读
            readerIndex_ += len;
        }
        else // len == readableBytes()
        {
            retrieveAll();
        }
    }

    void retrieveAll()
    {
        readerIndex_ = writerIndex_ = kCheapPrepend;
    }

    // 把onMessage函数上报的Buffer数据，转成string类型的数据返回
    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes()); // 应用可读取数据的长度
    }

    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 上面一句把缓冲区中可读的数据，已经读取出来，这里肯定要对缓冲区进行复位操作
        return result;
    }

    // buffer_.size() - writerIndex_    len
    void ensureWriteableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容函数
        }
    }

    // 把[data, data+len]内存上的数据，添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWriteableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    char *beginWrite()
    {
        return begin() + writerIndex_;
    }

    const char *beginWrite() const
    {
        return begin() + writerIndex_;
    }

    // 从fd上读取数据
    ssize_t readFd(int fd, int *saveErrno);
    // 通过fd发送数据
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // 获取vector底层数据起始地址
    char *begin()
    {
        // it.operator*() 调用了迭代器星号运算符重载函数，返回容器第0号元素本身，再使用&取地址
        return &*buffer_.begin(); // vector底层数组首元素的地址，也就是数组的起始地址
    }
    const char *begin() const
    {
        return &*buffer_.begin();
    }
    void makeSpace(size_t len)
    {
        /**
         * writableBytes：可写的
         * prependableBytes：读缓冲区空闲的，即已经被读完的，
         * writerIndex-readIndex这部分区域前置，
         * readIndex-kCheapPrepend这部分区域因为已经被读取，所以被后置在writer区域
         * 可写的加上读缓冲区空闲的都不能满足所需写空间则扩容
         *
         * kCheapPretend |  reader  |  writer  |
         * kCheapPretend |         len             |
         **/
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else
        {
            size_t readalbe = readableBytes();
            // 读缓冲区未读区域前置，writerIndex_-readerIndex_：未读区域，前置到数组下标kCheapPrepend
            std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readalbe;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};