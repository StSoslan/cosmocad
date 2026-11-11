#include <cctype>
#include <cmath>
#include <iomanip>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>

#include "cosmocad/core/Units.h"
#include "cosmocad/drawing/Entities.h"
#include "cosmocad/exchange/Exchange.h"

namespace cosmocad::exchange {

using core::ErrorCode;
using core::makeError;
using graphics::SceneItem;

namespace {

// Имена слоёв в DXF R12 — только латиница, цифры и знаки «_», «-», «$».
std::string dxfLayerName(const std::string& name, std::map<std::string, std::string>& cache) {
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    bool ascii = !name.empty();
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '$') out.push_back(static_cast<char>(std::toupper(c)));
        else if (c < 128) out.push_back('_');
        else ascii = false;
    }
    if (!ascii || out.empty()) out = "LAYER_" + std::to_string(cache.size() + 1);
    cache[name] = out;
    return out;
}

class Writer {
public:
    explicit Writer(std::ostream& out) : out_(out) { out_ << std::setprecision(15); }
    void pair(int code, const std::string& value) { out_ << code << '\n' << value << '\n'; }
    void pair(int code, double value) { out_ << code << '\n' << value << '\n'; }
    void pair(int code, int value) { out_ << code << '\n' << value << '\n'; }

private:
    std::ostream& out_;
};

std::string singleLine(std::string text) {
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ' ';
    return text;
}

}  // namespace

core::Result<void> writeDxf(const graphics::Scene& scene, std::ostream& out) {
    Writer w(out);
    std::map<std::string, std::string> layerNames;
    std::map<std::vector<double>, std::string> lineTypes;
    for (const auto& item : scene.items) {
        dxfLayerName(item.layer, layerNames);
        if (!item.pattern.empty() && !lineTypes.count(item.pattern))
            lineTypes[item.pattern] = "CC_PATTERN_" + std::to_string(lineTypes.size() + 1);
    }

    w.pair(0, "SECTION");
    w.pair(2, "HEADER");
    w.pair(9, "$ACADVER");
    w.pair(1, "AC1009");
    w.pair(0, "ENDSEC");

    w.pair(0, "SECTION");
    w.pair(2, "TABLES");
    w.pair(0, "TABLE");
    w.pair(2, "LTYPE");
    w.pair(70, static_cast<int>(lineTypes.size() + 1));
    w.pair(0, "LTYPE");
    w.pair(2, "CONTINUOUS");
    w.pair(70, 0);
    w.pair(3, "Solid line");
    w.pair(72, 65);
    w.pair(73, 0);
    w.pair(40, 0.0);
    for (const auto& [pattern, name] : lineTypes) {
        double total = 0.0;
        for (double v : pattern) total += v;
        w.pair(0, "LTYPE");
        w.pair(2, name);
        w.pair(70, 0);
        w.pair(3, "CosmoCad pattern");
        w.pair(72, 65);
        w.pair(73, static_cast<int>(pattern.size()));
        w.pair(40, total);
        for (std::size_t i = 0; i < pattern.size(); ++i) w.pair(49, i % 2 == 0 ? pattern[i] : -pattern[i]);
    }
    w.pair(0, "ENDTAB");
    w.pair(0, "TABLE");
    w.pair(2, "LAYER");
    w.pair(70, static_cast<int>(layerNames.size()));
    for (const auto& [original, name] : layerNames) {
        w.pair(0, "LAYER");
        w.pair(2, name);
        w.pair(70, 0);
        w.pair(62, 7);
        w.pair(6, "CONTINUOUS");
    }
    w.pair(0, "ENDTAB");
    w.pair(0, "ENDSEC");

    w.pair(0, "SECTION");
    w.pair(2, "ENTITIES");
    for (const auto& item : scene.items) {
        auto common = [&](const char* type) {
            w.pair(0, type);
            w.pair(8, dxfLayerName(item.layer, layerNames));
            if (!item.pattern.empty()) w.pair(6, lineTypes.at(item.pattern));
        };
        switch (item.kind) {
            case SceneItem::Kind::Line:
                common("LINE");
                w.pair(10, item.a.x);
                w.pair(20, item.a.y);
                w.pair(30, 0.0);
                w.pair(11, item.b.x);
                w.pair(21, item.b.y);
                w.pair(31, 0.0);
                break;
            case SceneItem::Kind::Circle:
                common("CIRCLE");
                w.pair(10, item.a.x);
                w.pair(20, item.a.y);
                w.pair(30, 0.0);
                w.pair(40, item.radius);
                break;
            case SceneItem::Kind::Arc:
                common("ARC");
                w.pair(10, item.a.x);
                w.pair(20, item.a.y);
                w.pair(30, 0.0);
                w.pair(40, item.radius);
                w.pair(50, core::radiansToDegrees(item.startAngle));
                w.pair(51, core::radiansToDegrees(item.endAngle));
                break;
            case SceneItem::Kind::Text:
                common("TEXT");
                w.pair(10, item.a.x);
                w.pair(20, item.a.y);
                w.pair(30, 0.0);
                w.pair(40, item.textHeight);
                w.pair(1, singleLine(item.text));
                if (std::abs(item.textAngle) > 1e-12) w.pair(50, core::radiansToDegrees(item.textAngle));
                if (item.textCentered) {
                    w.pair(72, 1);  // выравнивание по центру относительно второй точки
                    w.pair(11, item.a.x);
                    w.pair(21, item.a.y);
                    w.pair(31, 0.0);
                }
                break;
        }
    }
    w.pair(0, "ENDSEC");
    w.pair(0, "EOF");

    if (!out) return makeError(ErrorCode::IoError, "ошибка записи DXF");
    return {};
}

// ---- Чтение ----

namespace {

struct Pair {
    int code = 0;
    std::string value;
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
    return s.substr(start);
}

bool toDouble(const std::string& text, double& out) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> out;
    return !stream.fail() && std::isfinite(out);
}

}  // namespace

core::Result<DxfImport> readDxf(std::istream& in) {
    constexpr std::size_t kMaxPairs = 50'000'000;  // защита от чрезмерно больших файлов

    std::vector<Pair> pairs;
    std::string codeLine, valueLine;
    while (std::getline(in, codeLine)) {
        if (!std::getline(in, valueLine)) return makeError(ErrorCode::ParseError, "DXF: файл обрывается на коде группы");
        Pair pair;
        try {
            std::size_t used = 0;
            std::string code = trim(codeLine);
            pair.code = std::stoi(code, &used);
            if (used != code.size()) throw std::invalid_argument("code");
        } catch (const std::exception&) {
            return makeError(ErrorCode::ParseError, "DXF: недопустимый код группы");
        }
        pair.value = trim(valueLine);
        pairs.push_back(std::move(pair));
        if (pairs.size() > kMaxPairs) return makeError(ErrorCode::ParseError, "DXF: файл слишком велик");
    }
    if (pairs.empty()) return makeError(ErrorCode::ParseError, "DXF: пустой файл");

    // Ищем секцию ENTITIES.
    std::size_t i = 0;
    bool found = false;
    for (; i + 1 < pairs.size(); ++i) {
        if (pairs[i].code == 0 && pairs[i].value == "SECTION" && pairs[i + 1].code == 2 &&
            pairs[i + 1].value == "ENTITIES") {
            i += 2;
            found = true;
            break;
        }
    }
    if (!found) return makeError(ErrorCode::ParseError, "DXF: нет секции ENTITIES");

    DxfImport result;
    std::map<std::string, core::Uuid> layers;
    auto layerFor = [&](const std::string& name) -> core::Uuid {
        if (name.empty() || name == "0") return {};
        auto it = layers.find(name);
        if (it != layers.end()) return it->second;
        auto layer = std::make_unique<drawing::Layer>();
        layer->name = name;
        core::Uuid id = layer->id();
        layers[name] = id;
        result.objects.push_back(std::move(layer));
        return id;
    };

    while (i < pairs.size()) {
        if (pairs[i].code != 0) return makeError(ErrorCode::ParseError, "DXF: ожидается начало элемента");
        const std::string type = pairs[i].value;
        if (type == "ENDSEC" || type == "EOF") break;
        ++i;
        std::map<int, std::string> groups;
        while (i < pairs.size() && pairs[i].code != 0) {
            groups.emplace(pairs[i].code, pairs[i].value);  // первое значение кода побеждает
            ++i;
        }
        auto num = [&](int code, double fallback, bool required, bool& ok) {
            auto it = groups.find(code);
            if (it == groups.end()) {
                if (required) ok = false;
                return fallback;
            }
            double value = fallback;
            if (!toDouble(it->second, value)) ok = false;
            return value;
        };
        std::string layerName = groups.count(8) ? groups[8] : std::string();
        bool ok = true;

        if (type == "LINE") {
            auto e = std::make_unique<drawing::LineEntity>();
            e->start = {num(10, 0, true, ok), num(20, 0, true, ok)};
            e->end = {num(11, 0, true, ok), num(21, 0, true, ok)};
            if (!ok) return makeError(ErrorCode::ParseError, "DXF: повреждён элемент LINE");
            e->layerId = layerFor(layerName);
            result.objects.push_back(std::move(e));
        } else if (type == "CIRCLE") {
            auto e = std::make_unique<drawing::CircleEntity>();
            e->center = {num(10, 0, true, ok), num(20, 0, true, ok)};
            e->radius = num(40, 0, true, ok);
            if (!ok || e->radius < 0) return makeError(ErrorCode::ParseError, "DXF: повреждён элемент CIRCLE");
            e->layerId = layerFor(layerName);
            result.objects.push_back(std::move(e));
        } else if (type == "ARC") {
            auto e = std::make_unique<drawing::ArcEntity>();
            e->center = {num(10, 0, true, ok), num(20, 0, true, ok)};
            e->radius = num(40, 0, true, ok);
            e->startAngle = core::degreesToRadians(num(50, 0, true, ok));
            e->endAngle = core::degreesToRadians(num(51, 0, true, ok));
            if (!ok || e->radius < 0) return makeError(ErrorCode::ParseError, "DXF: повреждён элемент ARC");
            e->layerId = layerFor(layerName);
            result.objects.push_back(std::move(e));
        } else if (type == "TEXT") {
            auto e = std::make_unique<drawing::TextEntity>();
            e->position = {num(10, 0, true, ok), num(20, 0, true, ok)};
            e->height = num(40, 3.5, true, ok);
            e->text = groups.count(1) ? groups[1] : std::string();
            if (!ok) return makeError(ErrorCode::ParseError, "DXF: повреждён элемент TEXT");
            e->layerId = layerFor(layerName);
            result.objects.push_back(std::move(e));
        } else {
            ++result.skippedEntities;
        }
    }
    return result;
}

}  // namespace cosmocad::exchange
