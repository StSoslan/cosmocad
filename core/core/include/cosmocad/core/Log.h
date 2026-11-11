// Журнал работы программы. Пишется только локально; по сети ничего не отправляется.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace cosmocad::core {

enum class LogLevel { Debug, Info, Warning, Error };

const char* toString(LogLevel level);

class Logger {
public:
    using Sink = std::function<void(LogLevel, std::string_view module, std::string_view message)>;

    static Logger& instance();

    void setSink(Sink sink);          // пустой приёмник — вывод в stderr
    void setMinLevel(LogLevel level);
    void log(LogLevel level, std::string_view module, std::string_view message);

private:
    Logger() = default;
    std::mutex mutex_;
    Sink sink_;
    LogLevel minLevel_ = LogLevel::Info;
};

inline void logInfo(std::string_view module, std::string_view message) {
    Logger::instance().log(LogLevel::Info, module, message);
}
inline void logWarning(std::string_view module, std::string_view message) {
    Logger::instance().log(LogLevel::Warning, module, message);
}
inline void logError(std::string_view module, std::string_view message) {
    Logger::instance().log(LogLevel::Error, module, message);
}

}  // namespace cosmocad::core
