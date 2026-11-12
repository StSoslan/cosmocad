#include "cosmocad/libraries/Fasteners.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cosmocad::libraries {

using core::ErrorCode;
using core::makeError;
using core::Result;
using nlohmann::json;

namespace {

// Число без лишних нулей: 12 → «12», 1.25 → «1.25».
std::string plain(double v) {
    if (std::abs(v) < 1e-9) return "0";
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(9);
    out << v;
    std::string text = out.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

// Число для обозначения: десятичная запятая.
std::string shown(double v) {
    std::string text = plain(v);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

bool same(double a, double b) { return std::abs(a - b) < 1e-9; }

// Шестиугольник с размером «под ключ» S: формулы координат вершин для эскиза.
modeling::SketchShape hexagon(double S) {
    modeling::SketchShape shape;
    shape.kind = modeling::SketchShape::Kind::Polygon;
    const double radius = S / std::sqrt(3.0);  // радиус описанной окружности
    for (int i = 0; i < 6; ++i) {
        const double angle = std::numbers::pi / 3.0 * i;
        shape.values.push_back(plain(radius * std::cos(angle)));
        shape.values.push_back(plain(radius * std::sin(angle)));
    }
    return shape;
}

modeling::SketchShape circle(double diameter, bool hole = false) {
    modeling::SketchShape shape;
    shape.kind = modeling::SketchShape::Kind::Circle;
    shape.hole = hole;
    shape.values = {"0", "0", plain(diameter / 2.0)};
    return shape;
}

}  // namespace

Result<FastenerLibrary> FastenerLibrary::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть библиотеку крепежа " + file.string());
    try {
        const json j = json::parse(in);
        FastenerLibrary library;
        library.version_ = j.at("version").get<std::string>();
        library.source_ = j.value("source", std::string());
        const json& bolts = j.at("bolts");
        library.boltStandard_ = bolts.at("standard").get<std::string>();
        library.lengths_ = bolts.at("lengths").get<std::vector<double>>();
        library.lengthsNotRecommended_ = bolts.value("lengthsNotRecommended", std::vector<double>{});
        for (const auto& item : bolts.at("items")) {
            Bolt b;
            b.d = item.at("d").get<double>();
            b.pitch = item.at("pitch").get<double>();
            b.S = item.at("S").get<double>();
            b.k = item.at("k").get<double>();
            b.e = item.at("e").get<double>();
            b.lengthMin = item.at("lengthMin").get<double>();
            b.lengthMax = item.at("lengthMax").get<double>();
            b.fullThreadUpTo = item.at("fullThreadUpTo").get<double>();
            b.threadTo125 = item.at("threadTo125").get<double>();
            if (item.contains("threadTo200") && !item.at("threadTo200").is_null()) b.threadTo200 = item.at("threadTo200").get<double>();
            b.notRecommended = item.value("notRecommended", false);
            if (!(b.d > 0 && b.S > b.d && b.k > 0 && b.lengthMin > 0 && b.lengthMax >= b.lengthMin))
                return makeError(ErrorCode::ParseError, "библиотека крепежа: неверные размеры болта М" + shown(b.d));
            library.bolts_.push_back(b);
        }
        const json& nuts = j.at("nuts");
        library.nutStandard_ = nuts.at("standard").get<std::string>();
        for (const auto& item : nuts.at("items")) {
            Nut n{item.at("d").get<double>(), item.at("S").get<double>(), item.at("m").get<double>(), item.at("e").get<double>(),
                  item.value("notRecommended", false)};
            if (!(n.d > 0 && n.S > n.d && n.m > 0))
                return makeError(ErrorCode::ParseError, "библиотека крепежа: неверные размеры гайки М" + shown(n.d));
            library.nuts_.push_back(n);
        }
        const json& washers = j.at("washers");
        library.washerStandard_ = washers.at("standard").get<std::string>();
        for (const auto& item : washers.at("items")) {
            Washer w{item.at("d").get<double>(), item.at("d1").get<double>(), item.at("d2").get<double>(), item.at("s").get<double>()};
            if (!(w.d > 0 && w.d1 > w.d && w.d2 > w.d1 && w.s > 0))
                return makeError(ErrorCode::ParseError, "библиотека крепежа: неверные размеры шайбы " + shown(w.d));
            library.washers_.push_back(w);
        }
        return library;
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("библиотека крепежа: ") + e.what());
    }
}

std::vector<double> FastenerLibrary::boltLengths(double d) const {
    std::vector<double> out;
    for (const auto& bolt : bolts_)
        if (same(bolt.d, d))
            for (const double l : lengths_)
                if (l >= bolt.lengthMin - 1e-9 && l <= bolt.lengthMax + 1e-9) out.push_back(l);
    return out;
}

std::vector<double> FastenerLibrary::diameters(FastenerKind kind) const {
    std::vector<double> out;
    if (kind == FastenerKind::Bolt)
        for (const auto& b : bolts_) out.push_back(b.d);
    else if (kind == FastenerKind::Nut)
        for (const auto& n : nuts_) out.push_back(n.d);
    else
        for (const auto& w : washers_) out.push_back(w.d);
    return out;
}

Result<FastenerInfo> FastenerLibrary::describe(const FastenerRequest& request) const {
    FastenerInfo info;
    if (request.kind == FastenerKind::Bolt) {
        const auto it = std::find_if(bolts_.begin(), bolts_.end(), [&](const Bolt& b) { return same(b.d, request.d); });
        if (it == bolts_.end())
            return makeError(ErrorCode::NotFound, "в " + boltStandard_ + " нет болта с резьбой М" + shown(request.d));
        const auto lengths = boltLengths(request.d);
        if (std::none_of(lengths.begin(), lengths.end(), [&](double l) { return same(l, request.length); }))
            return makeError(ErrorCode::NotFound,
                             "в библиотеке нет болта М" + shown(request.d) + " длиной " + shown(request.length) +
                                 " мм: для этого диаметра длины от " + shown(it->lengthMin) + " до " + shown(it->lengthMax) +
                                 " мм из стандартного ряда");
        info.designation = "Болт М" + shown(request.d) + "×" + shown(request.length) + " " + boltStandard_;
        info.standard = boltStandard_;
        if (request.length <= it->fullThreadUpTo + 1e-9)
            info.threadLength = request.length;
        else if (request.length <= 125.0 + 1e-9)
            info.threadLength = it->threadTo125;
        else
            info.threadLength = it->threadTo200.value_or(it->threadTo125);
        info.notRecommended = it->notRecommended ||
                              std::any_of(lengthsNotRecommended_.begin(), lengthsNotRecommended_.end(),
                                          [&](double l) { return same(l, request.length); });
    } else if (request.kind == FastenerKind::Nut) {
        const auto it = std::find_if(nuts_.begin(), nuts_.end(), [&](const Nut& n) { return same(n.d, request.d); });
        if (it == nuts_.end()) return makeError(ErrorCode::NotFound, "в " + nutStandard_ + " нет гайки с резьбой М" + shown(request.d));
        info.designation = "Гайка М" + shown(request.d) + " " + nutStandard_;
        info.standard = nutStandard_;
        info.notRecommended = it->notRecommended;
    } else {
        const auto it = std::find_if(washers_.begin(), washers_.end(), [&](const Washer& w) { return same(w.d, request.d); });
        if (it == washers_.end())
            return makeError(ErrorCode::NotFound, "в библиотеке нет шайбы под резьбу " + shown(request.d) + " мм (" + washerStandard_ + ")");
        info.designation = "Шайба " + shown(request.d) + " " + washerStandard_;
        info.standard = washerStandard_;
    }
    return info;
}

Result<FastenerInfo> FastenerLibrary::build(modeling::IModelingService& modeling, document::Document& part,
                                            const FastenerRequest& request) const {
    auto info = describe(request);
    if (!info) return info;
    if (!modeling.features(part).empty()) return makeError(ErrorCode::InvalidState, "деталь уже содержит построения");
    const auto fail = [](const core::Error& error) { return Result<FastenerInfo>(error); };

    if (request.kind == FastenerKind::Bolt) {
        const Bolt& bolt = *std::find_if(bolts_.begin(), bolts_.end(), [&](const Bolt& b) { return same(b.d, request.d); });
        // Головка лежит под плоскостью XY, стержень идёт вверх: опорная плоскость головки — z = 0.
        auto head = modeling.addSketch(part, {"Головка", "XY", "0", "", {hexagon(bolt.S)}});
        if (!head) return fail(head.error());
        if (auto f = modeling.addExtrude(part, {head.value(), plain(bolt.k), false, true}); !f) return fail(f.error());
        auto shank = modeling.addSketch(part, {"Стержень", "XY", "0", "", {circle(bolt.d)}});
        if (!shank) return fail(shank.error());
        if (auto f = modeling.addExtrude(part, {shank.value(), plain(request.length), false, false}); !f) return fail(f.error());
    } else if (request.kind == FastenerKind::Nut) {
        const Nut& nut = *std::find_if(nuts_.begin(), nuts_.end(), [&](const Nut& n) { return same(n.d, request.d); });
        auto sketch = modeling.addSketch(part, {"Гайка", "XY", "0", "", {hexagon(nut.S), circle(nut.d, true)}});
        if (!sketch) return fail(sketch.error());
        if (auto f = modeling.addExtrude(part, {sketch.value(), plain(nut.m), false, false}); !f) return fail(f.error());
    } else {
        const Washer& washer = *std::find_if(washers_.begin(), washers_.end(), [&](const Washer& w) { return same(w.d, request.d); });
        auto sketch = modeling.addSketch(part, {"Шайба", "XY", "0", "", {circle(washer.d2), circle(washer.d1, true)}});
        if (!sketch) return fail(sketch.error());
        if (auto f = modeling.addExtrude(part, {sketch.value(), plain(washer.s), false, false}); !f) return fail(f.error());
    }
    if (auto ok = modeling.setPartProperties(part, info.value().designation, "Сталь", 7.85); !ok) return fail(ok.error());
    return info;
}

}  // namespace cosmocad::libraries
