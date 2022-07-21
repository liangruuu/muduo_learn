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
 * 还有一个关键点就是必须要先写才能读，即只有writeIndex动了，readIndex才能跟着动，writeIndex的值永远要比readIndex大
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

    // onMessage string <- Buffer，retrieve函数的功能就是把目前缓冲区中能够读取的数据给处理了
    void retrieve(size_t len)
    {
        // 这个if分支的情况出现在TcpConnection的handleWrite函数中
        if (len < readableBytes())
        {
            // 应用只读取了可读缓冲区数据的一部分，就是len，还剩下readerIndex_ += len和writerIndex_之间的数据没读
            readerIndex_ += len;
        }
        // len == readableBytes()，这个else分支的情况出现在retrieveAsString函数中
        else
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
        /**
         * 对于缓冲区可写入数据大小和想要写入的数据大小的比较，
         * 执行makspace函数的前提是write区域已经无法容所要要写入的数据，要么对其进行扩容，
         * 要么把read区域内的部分空间当作写空间，具体逻辑在makespace函数中有具体的解释
         */
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容函数
        }
    }

    // 把[data, data+len]内存上的数据，添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        // 确保写入空间能够塞进len大小的data数据
        ensureWriteableBytes(len);
        // 把len长度的data数据写入以beginWrite()返回值为起始地址的空间
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
         *                                             readIndex_
         * kCheapPretend |<- readIndex_ - kCheapPretend  ->|    reader  |  writer  |
         * kCheapPretend |                      len                                     |
         *
         * 对writableBytes() + prependableBytes()的解释：
         *
         * 上面的式子表示缓冲区能写入数据的空间，writableBytes()自然不用多说，本来就表示已经存在的可以写入的空间，
         * 而prependableBytes()的逻辑是返回readerIndex_，因为readerIndex_会随着不断读取内容而增大，
         * writerIndex_-readerIndex_表示的是未读区域，这部分空间是不能进行写操作的，因为仍有数据未被读取，
         * 而readerIndex_-kCheapPrepend这部分的区域表示的是已经被读取数据的空间，因为数据已经被读取，所以可以写入
         * 所以writableBytes() + prependableBytes()拆开来看就是:
         * writableBytes() + (readIndex_ - kCheapPrepend) + kCheapPrepend
         * 让其与len + kCheapPrepend比较，把右边的kCheapPrepend消掉，就等价于
         * writableBytes() + (readIndex_ - kCheapPrepend) < len
         *
         * len表示想要往缓冲写的数据大小，不等式左侧代表缓冲区现在能够写入的数据大小，
         * 所以我们可以看到这其实就是在比较想要写入的和目前所能提供的数据大小进行对比，
         *
         * makespace函数被调用的前提是writableBytes() < len，这个判断条件在ensureWriteableBytes函数中体现，
         * 意思是write区域的空间是肯定无法容纳想要写入的数据的，
         * 只有空间不足我们才需要调用扩容函数maksespace，但是这里还是进行了区分，因为虽然write区域无法容纳数据，
         * 但是read区域因为有些数据已经被读取了，所以这个被读取了的空间也可以被当作写空间，所以最后真正能够写入数据的空间
         * 就是我们之前所提到的writableBytes() + (readIndex_ - kCheapPrepend)，如果连加上read区域的这部分已被读取数据的空间
         * 还不能够满足写入数据的空间大小条件，则需要扩容；如果加上了read区域已被读取数据的空间能够满足写入空间条件的话
         * 则不需要扩容，只需要在原有的基础上对相关读写位置进行变换和数据的转移即可
         **/
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else
        {
            size_t readalbe = readableBytes();
            /**
             * 读缓冲区未读区域前置，writerIndex_-readerIndex_：未读区域，前置到数组下标kCheapPrepend
             *
             * 把从begin() + readerIndex_->begin() + writerIndex_之间的所有数据转移到以
             * begin() + kCheapPrepend开头的位置上去，这部分的空间代表还未读取的数据
             */
            std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readalbe;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};