#include "log.h"

// 全局日志配置（单例）
static LogConfig g_log_config = { 0 };

// 线程锁（保证多线程安全，Windows/Linux 兼容）
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_mutex;
#else
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// 初始化线程锁
static void log_mutex_init() {
#ifdef _WIN32
    InitializeCriticalSection(&g_log_mutex);
#endif
}

// 加锁
static void log_mutex_lock() {
#ifdef _WIN32
    EnterCriticalSection(&g_log_mutex);
#else
    pthread_mutex_lock(&g_log_mutex);
#endif
}

// 解锁
static void log_mutex_unlock() {
#ifdef _WIN32
    LeaveCriticalSection(&g_log_mutex);
#else
    pthread_mutex_unlock(&g_log_mutex);
#endif
}

// 销毁线程锁
static void log_mutex_destroy() {
#ifdef _WIN32
    DeleteCriticalSection(&g_log_mutex);
#endif
}

// 获取当前时间戳字符串（格式：YYYY-MM-DD HH:MM:SS）
static void get_time_str(char* buf, int buf_len) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", tm_info);
}

// 刷盘：将缓冲区内容写入文件
static void log_flush() {
    if (g_log_config.fp == NULL || g_log_config.buffer_pos == 0) {
        return;
    }

    // 写入文件
    fwrite(g_log_config.buffer, 1, g_log_config.buffer_pos, g_log_config.fp);
    fflush(g_log_config.fp);

    // 重置缓冲区
    g_log_config.buffer_pos = 0;
    memset(g_log_config.buffer, 0, g_log_config.buffer_size);

    // 更新刷盘时间
    g_log_config.last_flush_time = time(NULL);
}

// 初始化日志
int log_init(const char* log_path, LogLevel level, int buffer_size, int flush_interval) {
    // 校验参数
    if (log_path == NULL || strlen(log_path) == 0 || buffer_size <= 0) {
        return -1;
    }

    // 初始化线程锁
    log_mutex_init();

    // 加锁保护全局配置
    log_mutex_lock();

    // 关闭已有日志
    if (g_log_config.fp != NULL) {
        fclose(g_log_config.fp);
        g_log_config.fp = NULL;
    }

    // 打开日志文件（追加模式）
    g_log_config.fp = fopen(log_path, "a+");
    if (g_log_config.fp == NULL) {
        log_mutex_unlock();
        return -2;
    }

    // 设置配置
    strncpy(g_log_config.log_path, log_path, sizeof(g_log_config.log_path) - 1);
    g_log_config.level = level;
    g_log_config.flush_interval = flush_interval;
    g_log_config.last_flush_time = time(NULL);

    // 初始化缓冲区
    if (g_log_config.buffer != NULL) {
        free(g_log_config.buffer);
    }
    g_log_config.buffer = (char*)malloc(buffer_size);
    if (g_log_config.buffer == NULL) {
        fclose(g_log_config.fp);
        g_log_config.fp = NULL;
        log_mutex_unlock();
        return -3;
    }
    g_log_config.buffer_size = buffer_size;
    g_log_config.buffer_pos = 0;

    // 解锁
    log_mutex_unlock();

    // 输出初始化日志
    log_info("日志系统初始化成功，文件路径：%s，输出级别：%d，缓冲区大小：%d 字节，刷盘间隔：%d 秒",
        log_path, level, buffer_size, flush_interval);
    return 0;
}

// 关闭日志
void log_close() {
    log_mutex_lock();

    // 刷盘剩余内容
    log_flush();

    // 关闭文件
    if (g_log_config.fp != NULL) {
        fclose(g_log_config.fp);
        g_log_config.fp = NULL;
    }

    // 释放缓冲区
    if (g_log_config.buffer != NULL) {
        free(g_log_config.buffer);
        g_log_config.buffer = NULL;
    }

    log_mutex_unlock();

    // 销毁线程锁
    log_mutex_destroy();

    printf("日志系统已关闭\n");
}

// 日志写入核心函数
void log_write(LogLevel level, const char* func, int line, const char* format, ...) {
    // 过滤低于设置级别的日志
    if (level < g_log_config.level || g_log_config.fp == NULL) {
        return;
    }

    log_mutex_lock();

    // 检查是否需要刷盘（达到间隔/缓冲区满）
    time_t now = time(NULL);
    int need_flush = 0;
    if (g_log_config.flush_interval > 0) {
        if (now - g_log_config.last_flush_time >= g_log_config.flush_interval) {
            need_flush = 1;
        }
    }
    if (g_log_config.buffer_pos + 1024 >= g_log_config.buffer_size) { // 预留足够空间
        need_flush = 1;
    }
    if (need_flush) {
        log_flush();
    }

    // 1. 拼接时间戳
    char time_buf[32] = { 0 };
    get_time_str(time_buf, sizeof(time_buf));
    int pos = g_log_config.buffer_pos;
    pos += snprintf(g_log_config.buffer + pos, g_log_config.buffer_size - pos,
        "[%s] ", time_buf);

    // 2. 拼接日志级别
    const char* level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    pos += snprintf(g_log_config.buffer + pos, g_log_config.buffer_size - pos,
        "[%s] ", level_str[level]);

    // 3. 拼接函数名+行号
    pos += snprintf(g_log_config.buffer + pos, g_log_config.buffer_size - pos,
        "[%s:%d] ", func, line);

    // 4. 拼接日志内容（可变参数）
    va_list args;
    va_start(args, format);
    pos += vsnprintf(g_log_config.buffer + pos, g_log_config.buffer_size - pos,
        format, args);
    va_end(args);

    // 5. 换行
    pos += snprintf(g_log_config.buffer + pos, g_log_config.buffer_size - pos,
        "\n");

    // 更新缓冲区位置
    g_log_config.buffer_pos = pos;

    // 立即刷盘（如果间隔为0）
    if (g_log_config.flush_interval == 0) {
        log_flush();
    }

    log_mutex_unlock();
}