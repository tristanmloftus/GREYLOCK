#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <ctime>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) {
            file_.open("greylock.log", std::ios::app);
        }

        std::string level_str;
        switch (level) {
            case LogLevel::Debug:   level_str = "DEBUG"; break;
            case LogLevel::Info:    level_str = "INFO"; break;
            case LogLevel::Warning: level_str = "WARN"; break;
            case LogLevel::Error:   level_str = "ERROR"; break;
        }

        char timestamp[32];
        time_t now = time(nullptr);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

        file_ << "[" << timestamp << "] [" << level_str << "] " << message << std::endl;
    }

    void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
    void info(const std::string& msg) { log(LogLevel::Info, msg); }
    void warning(const std::string& msg) { log(LogLevel::Warning, msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }

    std::string get_last_error() const { return last_error_; }
    void set_last_error(const std::string& err) { last_error_ = err; }
    void clear_last_error() { last_error_.clear(); }

private:
    Logger() = default;
    ~Logger() { if (file_.is_open()) file_.close(); }

    std::ofstream file_;
    std::mutex mutex_;
    std::string last_error_;
};