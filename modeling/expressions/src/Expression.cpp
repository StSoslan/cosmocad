#include "cosmocad/modeling/Expression.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <set>

namespace cosmocad::modeling {

using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

const double kDegrees = std::numbers::pi / 180.0;

std::optional<double> unitFactor(std::string_view name) {
    if (name == "mm" || name == "мм") return 1.0;
    if (name == "cm" || name == "см") return 10.0;
    if (name == "m" || name == "м") return 1000.0;
    if (name == "in") return 25.4;
    if (name == "deg") return 1.0;
    if (name == "rad") return 180.0 / std::numbers::pi;
    return std::nullopt;
}

bool isFunction(std::string_view name) {
    static const std::set<std::string_view> names = {"sin", "cos", "tan", "asin", "acos", "atan",
                                                     "sqrt", "abs", "min", "max"};
    return names.count(name) > 0;
}

bool isNameStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool isNameChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

class Parser {
public:
    Parser(std::string_view text, const NameLookup* lookup, std::vector<std::string>* names)
        : text_(text), lookup_(lookup), names_(names) {}

    Result<double> run() {
        skipSpaces();
        if (pos_ >= text_.size()) return makeError(ErrorCode::ParseError, "формула пуста");
        auto value = sum();
        if (!value) return value;
        skipSpaces();
        if (pos_ < text_.size())
            return makeError(ErrorCode::ParseError,
                             "в формуле «" + std::string(text_) + "» лишний символ «" + currentSymbol() + "»");
        if (!std::isfinite(value.value()))
            return makeError(ErrorCode::InvalidArgument, "формула «" + std::string(text_) + "» даёт бесконечность");
        return value;
    }

private:
    std::string currentSymbol() const {
        // Символ целиком, даже если он занимает несколько байт (русская буква).
        std::size_t end = pos_ + 1;
        while (end < text_.size() && (static_cast<unsigned char>(text_[end]) & 0xC0) == 0x80) ++end;
        return std::string(text_.substr(pos_, end - pos_));
    }

    void skipSpaces() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool take(char c) {
        skipSpaces();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string_view readName() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && isNameChar(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        return text_.substr(start, pos_ - start);
    }

    Result<double> sum() {
        auto left = product();
        if (!left) return left;
        double value = left.value();
        while (true) {
            if (take('+')) {
                auto right = product();
                if (!right) return right;
                value += right.value();
            } else if (take('-')) {
                auto right = product();
                if (!right) return right;
                value -= right.value();
            } else {
                return value;
            }
        }
    }

    Result<double> product() {
        auto left = unary();
        if (!left) return left;
        double value = left.value();
        while (true) {
            if (take('*')) {
                auto right = unary();
                if (!right) return right;
                value *= right.value();
            } else if (take('/')) {
                auto right = unary();
                if (!right) return right;
                if (right.value() == 0.0 && lookup_)
                    return makeError(ErrorCode::InvalidArgument, "в формуле «" + std::string(text_) + "» деление на ноль");
                value = right.value() == 0.0 ? 0.0 : value / right.value();
            } else {
                return value;
            }
        }
    }

    Result<double> unary() {
        if (take('-')) {
            auto value = unary();
            if (!value) return value;
            return -value.value();
        }
        if (take('+')) return unary();
        return power();
    }

    Result<double> power() {
        auto base = primary();
        if (!base) return base;
        if (take('^')) {
            auto exponent = unary();
            if (!exponent) return exponent;
            return std::pow(base.value(), exponent.value());
        }
        return base;
    }

    Result<double> number() {
        const std::size_t start = pos_;
        std::string digits;
        bool seenSeparator = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                digits += c;
            } else if ((c == '.' || (c == ',' && argumentDepth_ == 0)) && !seenSeparator && pos_ + 1 < text_.size() &&
                       std::isdigit(static_cast<unsigned char>(text_[pos_ + 1]))) {
                digits += '.';
                seenSeparator = true;
            } else {
                break;
            }
            ++pos_;
        }
        if (pos_ == start) return makeError(ErrorCode::ParseError, "в формуле ожидалось число");
        double value = std::strtod(digits.c_str(), nullptr);
        // Единица измерения сразу после числа.
        const std::size_t beforeUnit = pos_;
        skipSpaces();
        if (pos_ < text_.size() && isNameStart(static_cast<unsigned char>(text_[pos_]))) {
            const std::string_view name = readName();
            if (const auto factor = unitFactor(name))
                value *= *factor;
            else
                pos_ = beforeUnit;
        } else {
            pos_ = beforeUnit;
        }
        return value;
    }

    Result<double> primary() {
        skipSpaces();
        if (pos_ >= text_.size())
            return makeError(ErrorCode::ParseError, "формула «" + std::string(text_) + "» обрывается");
        const unsigned char c = static_cast<unsigned char>(text_[pos_]);
        if (c == '(') {
            ++pos_;
            const int saved = argumentDepth_;
            argumentDepth_ = 0;
            auto value = sum();
            argumentDepth_ = saved;
            if (!value) return value;
            if (!take(')'))
                return makeError(ErrorCode::ParseError, "в формуле «" + std::string(text_) + "» не закрыта скобка");
            return value;
        }
        if (std::isdigit(c) || c == '.') return number();
        if (isNameStart(c)) {
            const std::string name(readName());
            if (name == "pi") return std::numbers::pi;
            if (isFunction(name)) return call(name);
            if (names_ && std::find(names_->begin(), names_->end(), name) == names_->end()) names_->push_back(name);
            if (!lookup_) return 1.0;
            if (const auto value = (*lookup_)(name)) return *value;
            return makeError(ErrorCode::NotFound, "в формуле «" + std::string(text_) + "» неизвестное имя «" + name + "»");
        }
        return makeError(ErrorCode::ParseError,
                         "в формуле «" + std::string(text_) + "» непонятный символ «" + currentSymbol() + "»");
    }

    Result<double> call(const std::string& name) {
        if (!take('('))
            return makeError(ErrorCode::ParseError, "после «" + name + "» в формуле нужна открывающая скобка");
        ++argumentDepth_;
        std::vector<double> args;
        do {
            auto value = sum();
            if (!value) {
                --argumentDepth_;
                return value;
            }
            args.push_back(value.value());
        } while (take(','));
        --argumentDepth_;
        if (!take(')'))
            return makeError(ErrorCode::ParseError, "в формуле «" + std::string(text_) + "» не закрыта скобка");
        const bool twoOrMore = name == "min" || name == "max";
        if ((twoOrMore && args.size() < 2) || (!twoOrMore && args.size() != 1))
            return makeError(ErrorCode::ParseError, "у функции «" + name + "» неверное число аргументов");
        const double x = args[0];
        if (name == "sin") return std::sin(x * kDegrees);
        if (name == "cos") return std::cos(x * kDegrees);
        if (name == "tan") return std::tan(x * kDegrees);
        if (name == "asin") return std::asin(x) / kDegrees;
        if (name == "acos") return std::acos(x) / kDegrees;
        if (name == "atan") return std::atan(x) / kDegrees;
        if (name == "abs") return std::abs(x);
        if (name == "sqrt") {
            if (x < 0.0 && lookup_)
                return makeError(ErrorCode::InvalidArgument, "в формуле «" + std::string(text_) + "» корень из отрицательного числа");
            return std::sqrt(std::max(x, 0.0));
        }
        if (name == "min") return *std::min_element(args.begin(), args.end());
        return *std::max_element(args.begin(), args.end());
    }

    std::string_view text_;
    const NameLookup* lookup_;
    std::vector<std::string>* names_;
    std::size_t pos_ = 0;
    int argumentDepth_ = 0;
};

}  // namespace

Result<double> evaluateExpression(std::string_view text, const NameLookup& lookup) {
    return Parser(text, &lookup, nullptr).run();
}

Result<std::vector<std::string>> expressionNames(std::string_view text) {
    std::vector<std::string> names;
    auto parsed = Parser(text, nullptr, &names).run();
    if (!parsed) return parsed.error();
    return names;
}

bool isValidParameterName(std::string_view name) {
    if (name.empty() || !isNameStart(static_cast<unsigned char>(name[0]))) return false;
    for (const char c : name)
        if (!isNameChar(static_cast<unsigned char>(c))) return false;
    return !isFunction(name) && !unitFactor(name) && name != "pi";
}

std::string formatNumber(double value) {
    // Без показателя степени: запись «1e-09» формулой не читается. Десять знаков после запятой,
    // лишние нули убираются; всё, что меньше, считается нулём.
    char buffer[400];
    std::snprintf(buffer, sizeof buffer, "%.10f", value);
    std::string text = buffer;
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    if (text == "-0") text = "0";
    return text;
}

}  // namespace cosmocad::modeling
