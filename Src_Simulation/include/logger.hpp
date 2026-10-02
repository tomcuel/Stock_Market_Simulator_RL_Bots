//=======================================================================
// Minimal thread-safe leveled logger: no external dependency (no {fmt}, no spdlog):
// Src_Simulation only relies on the standard library so it stays trivially buildable and unit-testable anywhere a C++20 compiler is available
//=======================================================================
#pragma once

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace sim {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

inline const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?";
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_min_level(LogLevel level) { min_level_ = level; }

    // optional: also mirror every log line to a file (e.g. logs/simulation.log)
    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.open(path, std::ios::out | std::ios::trunc);
    }

    template <typename... Args>
    void log(LogLevel level, Args&&... args) {
        if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

        std::ostringstream body;
        (body << ... << args); // fold expression: concatenate all arguments, no {fmt} dependency needed

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream line;
        line << "[" << std::put_time(std::localtime(&now_c), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << "] [" << to_string(level) << "] [tid " << std::this_thread::get_id() << "] " << body.str();

        std::lock_guard<std::mutex> lock(mutex_);
        std::ostream& sink = (level == LogLevel::ERROR || level == LogLevel::WARN) ? std::cerr : std::cout;
        sink << line.str() << std::endl;
        if (file_.is_open()) {
            file_ << line.str() << std::endl;
        }
    }

private:
    Logger() = default;
    LogLevel min_level_{LogLevel::INFO};
    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace sim

#define LOG_DEBUG(...) ::sim::Logger::instance().log(::sim::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  ::sim::Logger::instance().log(::sim::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...)  ::sim::Logger::instance().log(::sim::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) ::sim::Logger::instance().log(::sim::LogLevel::ERROR, __VA_ARGS__)
