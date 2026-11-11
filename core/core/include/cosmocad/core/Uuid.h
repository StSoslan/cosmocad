// Постоянный уникальный номер объекта (UUID версии 4).
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "cosmocad/core/Result.h"

namespace cosmocad::core {

class Uuid {
public:
    Uuid() = default;  // «пустой» номер

    static Uuid generate();
    static Result<Uuid> parse(std::string_view text);

    bool isNil() const;
    std::string toString() const;
    const std::array<std::uint8_t, 16>& bytes() const { return bytes_; }

    auto operator<=>(const Uuid&) const = default;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

}  // namespace cosmocad::core

template <>
struct std::hash<cosmocad::core::Uuid> {
    std::size_t operator()(const cosmocad::core::Uuid& id) const noexcept {
        std::size_t h = 1469598103934665603ull;
        for (auto b : id.bytes()) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};
