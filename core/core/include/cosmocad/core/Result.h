// CosmoCad — базовый тип результата операции: либо значение, либо ошибка.
// Через границы модулей исключения не передаются (см. docs/api/interfaces.md).
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cosmocad::core {

enum class ErrorCode {
    InvalidArgument,
    NotFound,
    AlreadyExists,
    InvalidState,
    IoError,
    ParseError,
    UnsupportedVersion,
    CorruptedData,
    Cancelled,
    NotImplemented,
    Internal,
};

const char* toString(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return data_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() {
        assert(ok());
        return std::get<0>(data_);
    }
    const T& value() const {
        assert(ok());
        return std::get<0>(data_);
    }
    T take() {
        assert(ok());
        return std::move(std::get<0>(data_));
    }
    const Error& error() const {
        assert(!ok());
        return std::get<1>(data_);
    }

private:
    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const {
        assert(!ok());
        return *error_;
    }

private:
    std::optional<Error> error_;
};

}  // namespace cosmocad::core
