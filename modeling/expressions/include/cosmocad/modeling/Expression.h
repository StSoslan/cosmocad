// Формулы для размеров и параметров: «2 * ширина + 5 mm», «max(a, b) / 2».
//
// Результат — число в миллиметрах (для длин) или градусах (для углов).
// Единицы пишутся сразу после числа: mm, cm, m, in, deg, rad.
// Десятичный разделитель — точка или запятая («12,5»); внутри скобок функции
// запятая разделяет аргументы, поэтому там дробные числа пишутся через точку.
// Функции: sin, cos, tan (угол в градусах), asin, acos, atan (результат в градусах),
// sqrt, abs, min, max. Константа: pi.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cosmocad/core/Result.h"

namespace cosmocad::modeling {

// Возвращает значение имени или пусто, если имя неизвестно.
using NameLookup = std::function<std::optional<double>(std::string_view)>;

core::Result<double> evaluateExpression(std::string_view text, const NameLookup& lookup);

// Имена параметров, от которых зависит формула (без функций, единиц и pi).
core::Result<std::vector<std::string>> expressionNames(std::string_view text);

// Годится ли строка как имя параметра: буквы, цифры, «_», не с цифры, не имя функции или единицы.
bool isValidParameterName(std::string_view name);

// Число в записи, удобной человеку и однозначной при чтении обратно: «12.5».
std::string formatNumber(double value);

}  // namespace cosmocad::modeling
