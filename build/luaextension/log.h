#ifndef _LOG_H_
#define	_LOG_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

// 日志级别
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

// 日志配置（全局单例）
typedef struct {
    FILE* fp;                // 日志文件句柄
    LogLevel level;          // 输出级别（低于该级别不输出）
    char log_path[256];      // 日志文件路径
    int buffer_size;         // 缓冲区大小（字节）
    char* buffer;            // 日志缓冲区
    int buffer_pos;          // 缓冲区当前位置
    int flush_interval;      // 刷盘间隔（秒），0 表示立即刷盘
    time_t last_flush_time;  // 上次刷盘时间
} LogConfig;

// 初始化日志（调用一次即可）
// 参数：log_path-日志文件路径，level-输出级别，buffer_size-缓冲区大小（建议4096），flush_interval-刷盘间隔（秒）
int log_init(const char* log_path, LogLevel level, int buffer_size, int flush_interval);

// 关闭日志（程序退出时调用）
void log_close();

// 日志输出核心函数（宏封装后更易用）
void log_write(LogLevel level, const char* func, int line, const char* format, ...);

// 宏封装：自动填充函数名、行号
#define log_debug(format, ...) log_write(LOG_DEBUG, __func__, __LINE__, format, ##__VA_ARGS__)
#define log_info(format, ...)  log_write(LOG_INFO,  __func__, __LINE__, format, ##__VA_ARGS__)
#define log_warn(format, ...)  log_write(LOG_WARN,  __func__, __LINE__, format, ##__VA_ARGS__)
#define log_error(format, ...) log_write(LOG_ERROR, __func__, __LINE__, format, ##__VA_ARGS__)

#endif // _LOG_H_