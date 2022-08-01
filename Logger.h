#pragma once

#include <string>

#include "noncopyable.h"

/**
 * 日志对于一个软件来说还是非常重要的，很多时候当软件正式应用以后使用gdb进行调试是不便的
 * 则日志是我们来处理问题最直接的途径
 *
 **/

// LOG_INFO("%s %d", arg1, arg2)
#define LOG_INFO(logmsgFormat, ...)                       \
    do {                                                  \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(INFO);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

#define LOG_ERROR(logmsgFormat, ...)                      \
    do {                                                  \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(ERROR);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

#define LOG_FATAL(logmsgFormat, ...)                      \
    do {                                                  \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(FATAL);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
        exit(-1);                                         \
    } while (0)

#ifdef MUDEBUG
#define LOG_DEBUG(logmsgFormat, ...)                      \
    do {                                                  \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(DEBUG);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)
#else
#define LOG_DEBUG(logmsgFormat, ...)
#endif

/**
 * 定义日志的级别  INFO  ERROR  FATAL  DEBUG
 * INFO：打印一些重要的流程信息
 * ERROR：这些错误是不影响软件继续向下执行的，不是说出现ERROR就必须exit
 * FATAL：这种问题出现则代表系统无法继续向下运行的，就必须输出重要的信息然后exit
 * DEGUG：调试信息，一般而言调试信息是非常多的，在系统正常运行的情况下会默认吧DEBUG日志关掉
 * 当需要去输出DEBUG日志的时候才打开一个开关，比如配置文件中的一个设置、或者是全局设置一个宏，当需要用到DEBUG信息的时候就打开这个宏，否则关闭
 *
 **/
enum LogLevel {
    INFO,   // 普通信息
    ERROR,  // 错误信息
    FATAL,  // core信息
    DEBUG,  // 调试信息
};

// 输出一个日志类
class Logger : noncopyable {
   public:
    // 获取日志唯一的实例对象
    static Logger &instance();
    // 设置日志级别
    void setLogLevel(int level);
    // 写日志
    void log(std::string msg);

   private:
    int logLevel_;
};