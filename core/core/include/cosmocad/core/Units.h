// Единицы измерения. Внутри программы длины хранятся в миллиметрах, углы — в радианах.
#pragma once

#include <numbers>

namespace cosmocad::core {

enum class LengthUnit { Millimeter, Centimeter, Meter, Inch };

constexpr double millimetersPer(LengthUnit unit) {
    switch (unit) {
        case LengthUnit::Millimeter: return 1.0;
        case LengthUnit::Centimeter: return 10.0;
        case LengthUnit::Meter: return 1000.0;
        case LengthUnit::Inch: return 25.4;
    }
    return 1.0;
}

constexpr double toMillimeters(double value, LengthUnit unit) { return value * millimetersPer(unit); }
constexpr double fromMillimeters(double mm, LengthUnit unit) { return mm / millimetersPer(unit); }

constexpr double degreesToRadians(double deg) { return deg * std::numbers::pi / 180.0; }
constexpr double radiansToDegrees(double rad) { return rad * 180.0 / std::numbers::pi; }

}  // namespace cosmocad::core
