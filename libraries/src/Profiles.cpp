#include "cosmocad/libraries/Profiles.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
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

std::string shown(double v) {
    std::string text = plain(v);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

bool same(double a, double b) { return std::abs(a - b) < 1e-9; }

// Дуга от угла from до угла to (радианы) вокруг центра, без первой точки; отрезков — parts.
void arc(std::vector<std::string>& values, double cx, double cy, double radius, double from, double to, int parts) {
    for (int i = 1; i <= parts; ++i) {
        const double angle = from + (to - from) * i / parts;
        values.push_back(plain(cx + radius * std::cos(angle)));
        values.push_back(plain(cy + radius * std::sin(angle)));
    }
}

}  // namespace

Result<ProfileLibrary> ProfileLibrary::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть библиотеку проката " + file.string());
    ProfileLibrary library;
    try {
        const json j = json::parse(in);
        const auto& angles = j.at("angles");
        library.angleStandard_ = angles.at("standard").get<std::string>();
        for (const auto& item : angles.at("items")) {
            AngleSize a;
            a.b = item.at("b").get<double>();
            a.t = item.at("t").get<double>();
            a.R = item.at("R").get<double>();
            a.r = item.at("r").get<double>();
            a.area = item.at("area").get<double>();
            a.mass = item.at("mass").get<double>();
            if (!(a.b > 0) || !(a.t > 0) || a.t >= a.b || a.R < 0 || a.r < 0 || a.r > a.t || a.t + a.R >= a.b)
                return makeError(ErrorCode::ParseError, "библиотека проката: неверные размеры уголка " + shown(a.b) + "×" + shown(a.t));
            library.angles_.push_back(a);
        }
        if (j.contains("channels")) {
            const auto& channels = j.at("channels");
            library.channelStandard_ = channels.at("standard").get<std::string>();
            for (const auto& item : channels.at("items")) {
                ChannelSize c;
                c.name = item.at("name").get<std::string>();
                c.series = item.value("series", std::string());
                c.h = item.at("h").get<double>();
                c.b = item.at("b").get<double>();
                c.s = item.at("s").get<double>();
                c.t = item.at("t").get<double>();
                c.R = item.at("R").get<double>();
                c.r = item.at("r").get<double>();
                c.area = item.at("area").get<double>();
                c.mass = item.at("mass").get<double>();
                if (c.name.empty() || !(c.h > 0) || !(c.b > c.s) || !(c.s > 0) || !(c.t > 0) || c.R < 0 || c.r < 0 || c.r > c.t ||
                    2 * (c.t + c.R) >= c.h || c.s + c.R + c.r >= c.b)
                    return makeError(ErrorCode::ParseError, "библиотека проката: неверные размеры швеллера " + c.name);
                library.channels_.push_back(std::move(c));
            }
        }
        if (j.contains("beams")) {
            const auto& beams = j.at("beams");
            library.beamStandard_ = beams.at("standard").get<std::string>();
            for (const auto& item : beams.at("items")) {
                BeamSize m;
                m.name = item.at("name").get<std::string>();
                m.type = item.at("type").get<std::string>();
                m.h = item.at("h").get<double>();
                m.b = item.at("b").get<double>();
                m.s = item.at("s").get<double>();
                m.t = item.at("t").get<double>();
                m.r = item.at("r").get<double>();
                m.area = item.at("area").get<double>();
                m.mass = item.value("mass", 0.0);
                if (m.name.empty() || !(m.h > 0) || !(m.b > m.s) || !(m.s > 0) || !(m.t > 0) || m.r < 0 || 2 * (m.t + m.r) >= m.h ||
                    m.s + 2 * m.r >= m.b)
                    return makeError(ErrorCode::ParseError, "библиотека проката: неверные размеры двутавра " + m.name);
                if (std::none_of(library.beamTypes_.begin(), library.beamTypes_.end(), [&](const auto& t) { return t.first == m.type; }))
                    library.beamTypes_.emplace_back(m.type, beams.contains("types") ? beams.at("types").value(m.type, m.type) : m.type);
                library.beams_.push_back(std::move(m));
            }
        }
    } catch (const json::exception& problem) {
        return makeError(ErrorCode::ParseError, "библиотека проката " + file.string() + ": " + problem.what());
    }
    return library;
}

std::vector<double> ProfileLibrary::angleWidths() const {
    std::vector<double> out;
    for (const auto& a : angles_)
        if (out.empty() || !same(out.back(), a.b)) out.push_back(a.b);
    return out;
}

std::vector<double> ProfileLibrary::angleThicknesses(double b) const {
    std::vector<double> out;
    for (const auto& a : angles_)
        if (same(a.b, b)) out.push_back(a.t);
    return out;
}

const AngleSize* ProfileLibrary::angle(double b, double t) const {
    for (const auto& a : angles_)
        if (same(a.b, b) && same(a.t, t)) return &a;
    return nullptr;
}

Result<std::string> ProfileLibrary::angleDesignation(double b, double t, double length) const {
    if (!angle(b, t)) return makeError(ErrorCode::NotFound, "в " + angleStandard_ + " нет уголка " + shown(b) + "×" + shown(b) + "×" + shown(t));
    if (!(length > 0) || !std::isfinite(length)) return makeError(ErrorCode::InvalidArgument, "длина уголка должна быть больше нуля");
    return "Уголок " + shown(b) + "×" + shown(b) + "×" + shown(t) + " " + angleStandard_ + " L=" + shown(length);
}

Result<void> ProfileLibrary::buildAngle(modeling::IModelingService& modeling, document::Document& part, double b, double t, double length) const {
    auto name = angleDesignation(b, t, length);
    if (!name) return name.error();
    if (!modeling.features(part).empty()) return makeError(ErrorCode::InvalidState, "деталь уже содержит построения");
    const AngleSize& a = *angle(b, t);
    const double pi = std::numbers::pi;
    const int parts = 8;
    // Контур против часовой стрелки от наружного угла; полки идут вдоль осей X и Y.
    modeling::SketchShape shape;
    shape.kind = modeling::SketchShape::Kind::Polygon;
    auto& v = shape.values;
    const auto point = [&](double x, double y) {
        v.push_back(plain(x));
        v.push_back(plain(y));
    };
    point(0, 0);
    point(a.b, 0);
    if (a.r > 0) {
        point(a.b, a.t - a.r);
        arc(v, a.b - a.r, a.t - a.r, a.r, 0.0, pi / 2, parts);
    } else {
        point(a.b, a.t);
    }
    if (a.R > 0) {
        point(a.t + a.R, a.t);
        arc(v, a.t + a.R, a.t + a.R, a.R, 1.5 * pi, pi, parts);
    } else {
        point(a.t, a.t);
    }
    if (a.r > 0) {
        point(a.t, a.b - a.r);
        arc(v, a.t - a.r, a.b - a.r, a.r, 0.0, pi / 2, parts);
    } else {
        point(a.t, a.b);
    }
    point(0, a.b);
    auto sketch = modeling.addSketch(part, {"Сечение уголка", "XY", "0", "", {shape}});
    if (!sketch) return sketch.error();
    if (auto f = modeling.addExtrude(part, {sketch.value(), plain(length), false, false}); !f) return f.error();
    if (auto ok = modeling.setPartProperties(part, name.value(), "Сталь", 7.85); !ok) return ok.error();
    return {};
}

const ChannelSize* ProfileLibrary::channel(const std::string& name) const {
    for (const auto& c : channels_)
        if (c.name == name) return &c;
    return nullptr;
}

const BeamSize* ProfileLibrary::beam(const std::string& name) const {
    for (const auto& m : beams_)
        if (m.name == name) return &m;
    return nullptr;
}

Result<std::string> ProfileLibrary::designation(const std::string& kind, const std::string& name, double length) const {
    if (!(length > 0) || !std::isfinite(length)) return makeError(ErrorCode::InvalidArgument, "длина отрезка должна быть больше нуля");
    if (kind == "channel") {
        if (!channel(name)) return makeError(ErrorCode::NotFound, "в библиотеке нет швеллера «" + name + "» (" + channelStandard_ + ")");
        return "Швеллер " + name + " " + channelStandard_ + " L=" + shown(length);
    }
    if (kind == "beam") {
        if (!beam(name)) return makeError(ErrorCode::NotFound, "в библиотеке нет двутавра «" + name + "» (" + beamStandard_ + ")");
        return "Двутавр " + name + " " + beamStandard_ + " L=" + shown(length);
    }
    return makeError(ErrorCode::InvalidArgument, "неизвестный вид проката «" + kind + "»: channel или beam");
}

Result<void> ProfileLibrary::build(modeling::IModelingService& modeling, document::Document& part, const std::string& kind,
                                   const std::string& name, double length) const {
    auto title = designation(kind, name, length);
    if (!title) return title.error();
    if (!modeling.features(part).empty()) return makeError(ErrorCode::InvalidState, "деталь уже содержит построения");
    const double pi = std::numbers::pi;
    const int parts = 8;
    modeling::SketchShape shape;
    shape.kind = modeling::SketchShape::Kind::Polygon;
    auto& v = shape.values;
    const auto point = [&](double x, double y) {
        v.push_back(plain(x));
        v.push_back(plain(y));
    };
    // Закругление: до начала дуги идёт точка, дальше — дуга короткими отрезками; при нулевом радиусе — просто угол.
    const auto round = [&](double cornerX, double cornerY, double cx, double cy, double radius, double from, double to) {
        if (radius > 0) {
            point(cx + radius * std::cos(from), cy + radius * std::sin(from));
            arc(v, cx, cy, radius, from, to, parts);
        } else {
            point(cornerX, cornerY);
        }
    };
    if (kind == "channel") {
        // Стенка вдоль оси Y от начала координат, полки — в сторону +X. Обход против часовой стрелки.
        const ChannelSize& c = *channel(name);
        point(0, 0);
        point(c.b, 0);
        round(c.b, c.t, c.b - c.r, c.t - c.r, c.r, 0.0, pi / 2);
        round(c.s, c.t, c.s + c.R, c.t + c.R, c.R, 1.5 * pi, pi);
        round(c.s, c.h - c.t, c.s + c.R, c.h - c.t - c.R, c.R, pi, pi / 2);
        round(c.b, c.h - c.t, c.b - c.r, c.h - c.t + c.r, c.r, 1.5 * pi, 2 * pi);
        point(c.b, c.h);
        point(0, c.h);
    } else {
        // Центр сечения — в начале координат, стенка вдоль оси Y.
        const BeamSize& m = *beam(name);
        const double w = m.b / 2, e = m.h / 2, half = m.s / 2;
        point(-w, -e);
        point(w, -e);
        point(w, -e + m.t);
        round(half, -e + m.t, half + m.r, -e + m.t + m.r, m.r, 1.5 * pi, pi);
        round(half, e - m.t, half + m.r, e - m.t - m.r, m.r, pi, pi / 2);
        point(w, e - m.t);
        point(w, e);
        point(-w, e);
        point(-w, e - m.t);
        round(-half, e - m.t, -half - m.r, e - m.t - m.r, m.r, pi / 2, 0.0);
        round(-half, -e + m.t, -half - m.r, -e + m.t + m.r, m.r, 0.0, -pi / 2);
        point(-w, -e + m.t);
    }
    auto sketch = modeling.addSketch(part, {kind == "channel" ? "Сечение швеллера" : "Сечение двутавра", "XY", "0", "", {shape}});
    if (!sketch) return sketch.error();
    if (auto f = modeling.addExtrude(part, {sketch.value(), plain(length), false, false}); !f) return f.error();
    if (auto ok = modeling.setPartProperties(part, title.value(), "Сталь", 7.85); !ok) return ok.error();
    return {};
}

}  // namespace cosmocad::libraries
