#pragma once

#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace stream_tablet {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static void set_level(LogLevel level) { s_level = level; }

    static void debug(const char* fmt, ...) {
        if (s_level <= LogLevel::DEBUG) {
            va_list args;
            va_start(args, fmt);
            log_impl("DEBUG", fmt, args);
            va_end(args);
        }
    }

    static void info(const char* fmt, ...) {
        if (s_level <= LogLevel::INFO) {
            va_list args;
            va_start(args, fmt);
            log_impl("INFO", fmt, args);
            va_end(args);
        }
    }

    static void warn(const char* fmt, ...) {
        if (s_level <= LogLevel::WARN) {
            va_list args;
            va_start(args, fmt);
            log_impl("WARN", fmt, args);
            va_end(args);
        }
    }

    static void error(const char* fmt, ...) {
        if (s_level <= LogLevel::ERROR) {
            va_list args;
            va_start(args, fmt);
            log_impl("ERROR", fmt, args);
            va_end(args);
        }
    }

private:
    static inline LogLevel s_level = LogLevel::INFO;

    static void log_impl(const char* level, const char* fmt, va_list args) {
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        char time_buf[20];
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

        fprintf(stderr, "[%s] [%s] ", time_buf, level);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    }
};

#define LOG_DEBUG(...) stream_tablet::Logger::debug(__VA_ARGS__)
#define LOG_INFO(...)  stream_tablet::Logger::info(__VA_ARGS__)
#define LOG_WARN(...)  stream_tablet::Logger::warn(__VA_ARGS__)
#define LOG_ERROR(...) stream_tablet::Logger::error(__VA_ARGS__)

}  // namespace stream_tablet
