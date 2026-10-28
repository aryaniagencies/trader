#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string_view>

namespace trader {

enum class LogLevel {
    info,
    warning,
    error,
};

inline void log(LogLevel level, std::string_view message)
{
    static std::mutex output_mutex;
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_r(&time, &local_time);

    const char* level_name = "INFO";
    if (level == LogLevel::warning) {
        level_name = "WARN";
    } else if (level == LogLevel::error) {
        level_name = "ERROR";
    }

    std::lock_guard lock(output_mutex);
    std::clog << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S")
              << " [" << level_name << "] " << message << '\n';
}

} // namespace trader