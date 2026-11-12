#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <cstdio>
#include <iostream>
#include <random>

#include "cosmocad/core/Log.h"
#include "cosmocad/core/Result.h"
#include "cosmocad/core/Uuid.h"

namespace cosmocad::core {

const char* toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::ParseError: return "ParseError";
        case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
        case ErrorCode::CorruptedData: return "CorruptedData";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

// ---- Uuid ----

Uuid Uuid::generate() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    Uuid id;
    for (int half = 0; half < 2; ++half) {
        std::uint64_t value = engine();
        for (int i = 0; i < 8; ++i) id.bytes_[half * 8 + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    id.bytes_[6] = static_cast<std::uint8_t>((id.bytes_[6] & 0x0F) | 0x40);  // версия 4
    id.bytes_[8] = static_cast<std::uint8_t>((id.bytes_[8] & 0x3F) | 0x80);  // вариант RFC 4122
    return id;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Result<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != 36) return makeError(ErrorCode::ParseError, "UUID: ожидается 36 символов");
    Uuid id;
    std::size_t byteIndex = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return makeError(ErrorCode::ParseError, "UUID: ожидается дефис");
            ++i;
            continue;
        }
        int hi = hexValue(text[i]);
        int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return makeError(ErrorCode::ParseError, "UUID: недопустимый символ");
        id.bytes_[byteIndex++] = static_cast<std::uint8_t>(hi * 16 + lo);
        i += 2;
    }
    return id;
}

bool Uuid::isNil() const {
    for (auto b : bytes_)
        if (b != 0) return false;
    return true;
}

std::string Uuid::toString() const {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(digits[bytes_[i] >> 4]);
        out.push_back(digits[bytes_[i] & 0x0F]);
    }
    return out;
}

// ---- Logger ----

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setSink(Sink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::setMinLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    minLevel_ = level;
}

void Logger::log(LogLevel level, std::string_view module, std::string_view message) {
    std::lock_guard lock(mutex_);
    if (level < minLevel_) return;
    if (sink_) {
        sink_(level, module, message);
        return;
    }
    std::cerr << '[' << toString(level) << "] " << module << ": " << message << '\n';
}

}  // namespace cosmocad::core
