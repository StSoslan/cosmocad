#include "cosmocad/drawing/Entities.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numbers>

#include <nlohmann/json.hpp>

namespace cosmocad::drawing {

using core::ErrorCode;
using core::makeError;
using nlohmann::json;

namespace {

json pointToJson(Point2 p) { return json::array({p.x, p.y}); }

Point2 pointFromJson(const json& j) { return Point2{j.at(0).get<double>(), j.at(1).get<double>()}; }

template <class F>
Result<void> guarded(const char* what, F&& body) {
    try {
        body();
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string(what) + ": " + e.what());
    }
}

}  // namespace

// ---- Layer ----

void Layer::writeJson(json& j) const {
    j["visible"] = visible;
    j["locked"] = locked;
    j["color"] = color;
    j["lineTypeId"] = lineTypeId;
    j["lineWidth"] = lineWidth;
}

Result<void> Layer::readJson(const json& j) {
    return guarded("слой", [&] {
        visible = j.value("visible", true);
        locked = j.value("locked", false);
        color = j.value("color", std::string("#000000"));
        lineTypeId = j.value("lineTypeId", std::string("solid_main"));
        lineWidth = j.value("lineWidth", 0.0);
    });
}

// ---- Sheet ----

void Sheet::writeJson(json& j) const {
    j["format"] = format;
    j["landscape"] = landscape;
    j["mainLineWidth"] = mainLineWidth;
    j["titleBlock"] = titleBlock;
}

Result<void> Sheet::readJson(const json& j) {
    return guarded("лист", [&] {
        format = j.value("format", std::string("A4"));
        landscape = j.value("landscape", false);
        mainLineWidth = j.value("mainLineWidth", 0.8);
        titleBlock = j.value("titleBlock", std::map<std::string, std::string>{});
    });
}

// ---- Entity ----

void Entity::writeEntityJson(json& j) const {
    if (!layerId.isNil()) j["layer"] = layerId.toString();
    if (lineTypeId) j["lineTypeId"] = *lineTypeId;
    if (lineWidth) j["lineWidth"] = *lineWidth;
}

Result<void> Entity::readEntityJson(const json& j) {
    layerId = Uuid();
    lineTypeId.reset();
    lineWidth.reset();
    if (j.contains("layer")) {
        auto id = Uuid::parse(j.at("layer").get<std::string>());
        if (!id) return id.error();
        layerId = id.value();
    }
    if (j.contains("lineTypeId")) lineTypeId = j.at("lineTypeId").get<std::string>();
    if (j.contains("lineWidth")) lineWidth = j.at("lineWidth").get<double>();
    return {};
}

// ---- Line ----

BBox2 LineEntity::bounds() const { return BBox2::fromCorners(start, end); }

void LineEntity::writeJson(json& j) const {
    writeEntityJson(j);
    j["start"] = pointToJson(start);
    j["end"] = pointToJson(end);
}

Result<void> LineEntity::readJson(const json& j) {
    return guarded("отрезок", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        start = pointFromJson(j.at("start"));
        end = pointFromJson(j.at("end"));
    });
}

// ---- Circle ----

BBox2 CircleEntity::bounds() const {
    return BBox2::fromCorners({center.x - radius, center.y - radius}, {center.x + radius, center.y + radius});
}

void CircleEntity::writeJson(json& j) const {
    writeEntityJson(j);
    j["center"] = pointToJson(center);
    j["radius"] = radius;
}

Result<void> CircleEntity::readJson(const json& j) {
    return guarded("окружность", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        center = pointFromJson(j.at("center"));
        radius = j.at("radius").get<double>();
    });
}

// ---- Arc ----

BBox2 ArcEntity::bounds() const { return geometry::arcBounds(center, radius, startAngle, endAngle); }

void ArcEntity::writeJson(json& j) const {
    writeEntityJson(j);
    j["center"] = pointToJson(center);
    j["radius"] = radius;
    j["startAngle"] = startAngle;
    j["endAngle"] = endAngle;
}

Result<void> ArcEntity::readJson(const json& j) {
    return guarded("дуга", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        center = pointFromJson(j.at("center"));
        radius = j.at("radius").get<double>();
        startAngle = j.at("startAngle").get<double>();
        endAngle = j.at("endAngle").get<double>();
    });
}

// ---- Text ----

BBox2 TextEntity::bounds() const {
    // Оценка: средняя ширина знака — 0,6 высоты. Точная ширина зависит от шрифта.
    // Считаем знаки, а не байты: в UTF-8 кириллическая буква занимает два байта.
    double width = 0.6 * height * static_cast<double>(characterCount(text));
    return BBox2::fromCorners(position, {position.x + width, position.y + height});
}

void TextEntity::writeJson(json& j) const {
    writeEntityJson(j);
    j["position"] = pointToJson(position);
    j["text"] = text;
    j["height"] = height;
}

Result<void> TextEntity::readJson(const json& j) {
    return guarded("текст", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        position = pointFromJson(j.at("position"));
        text = j.at("text").get<std::string>();
        height = j.at("height").get<double>();
    });
}

// ---- LinearDimension ----

BBox2 LinearDimension::bounds() const {
    BBox2 box = BBox2::fromCorners(p1, p2);
    Point2 direction = p2 - p1;
    double len = geometry::length(direction);
    if (len > geometry::kLinearTolerance) {
        Point2 normal{-direction.y / len, direction.x / len};
        box.expand(p1 + normal * offset);
        box.expand(p2 + normal * offset);
    }
    return box;
}

void LinearDimension::writeJson(json& j) const {
    writeEntityJson(j);
    j["p1"] = pointToJson(p1);
    j["p2"] = pointToJson(p2);
    j["offset"] = offset;
    j["textHeight"] = textHeight;
    note.write(j);
}

Result<void> LinearDimension::readJson(const json& j) {
    return guarded("размер", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        p1 = pointFromJson(j.at("p1"));
        p2 = pointFromJson(j.at("p2"));
        offset = j.at("offset").get<double>();
        textHeight = j.value("textHeight", 3.5);
        note.read(j);
    });
}

// ---- Преобразования ----

void LineEntity::transform(const geometry::Transform2& t) {
    start = t.apply(start);
    end = t.apply(end);
}

void CircleEntity::transform(const geometry::Transform2& t) {
    center = t.apply(center);
    radius *= t.scale();
}

void ArcEntity::transform(const geometry::Transform2& t) {
    center = t.apply(center);
    radius *= t.scale();
    if (t.mirrored) {
        // Отражение меняет направление обхода: начало и конец дуги меняются местами.
        double newStart = t.angle() - endAngle;
        double newEnd = t.angle() - startAngle;
        startAngle = geometry::normalizeAngle(newStart);
        endAngle = geometry::normalizeAngle(newEnd);
    } else {
        startAngle = geometry::normalizeAngle(startAngle + t.angle());
        endAngle = geometry::normalizeAngle(endAngle + t.angle());
    }
}

void TextEntity::transform(const geometry::Transform2& t) {
    // Текст переносится и масштабируется, но остаётся горизонтальным: поворота у текста пока нет.
    position = t.apply(position);
    height *= t.scale();
}

void LinearDimension::transform(const geometry::Transform2& t) {
    p1 = t.apply(p1);
    p2 = t.apply(p2);
    offset *= t.mirrored ? -t.scale() : t.scale();
}

// ---- DimensionNote ----

void DimensionNote::write(json& j) const {
    if (!suffix.empty()) j["suffix"] = suffix;
    if (!upper.empty()) j["upperDeviation"] = upper;
    if (!lower.empty()) j["lowerDeviation"] = lower;
}

void DimensionNote::read(const json& j) {
    suffix = j.value("suffix", std::string());
    upper = j.value("upperDeviation", std::string());
    lower = j.value("lowerDeviation", std::string());
}

std::size_t characterCount(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++count;
    return count;
}

// ---- Leader ----

Point2 Leader::shelfEnd() const {
    double length = 0.6 * textHeight * static_cast<double>(characterCount(text)) + 2.0;
    double direction = knee.x >= tip.x ? 1.0 : -1.0;
    return {knee.x + direction * length, knee.y};
}

BBox2 Leader::bounds() const {
    BBox2 box = BBox2::fromCorners(tip, knee);
    Point2 end = shelfEnd();
    box.expand(end);
    box.expand(Point2{end.x, end.y + textHeight + 1.0});
    return box;
}

void Leader::transform(const geometry::Transform2& t) {
    tip = t.apply(tip);
    knee = t.apply(knee);
    textHeight *= t.scale();
}

void Leader::writeJson(json& j) const {
    writeEntityJson(j);
    j["tip"] = pointToJson(tip);
    j["knee"] = pointToJson(knee);
    j["text"] = text;
    j["textHeight"] = textHeight;
}

Result<void> Leader::readJson(const json& j) {
    return guarded("выноска", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        tip = pointFromJson(j.at("tip"));
        knee = pointFromJson(j.at("knee"));
        text = j.at("text").get<std::string>();
        textHeight = j.value("textHeight", 3.5);
    });
}

// ---- Блоки ----

namespace {

json primitivesToJson(const std::vector<Primitive>& list) {
    json out = json::array();
    for (const auto& p : list) {
        json item;
        switch (p.kind) {
            case Primitive::Kind::Line: item = {{"k", "line"}, {"a", pointToJson(p.a)}, {"b", pointToJson(p.b)}}; break;
            case Primitive::Kind::Circle: item = {{"k", "circle"}, {"a", pointToJson(p.a)}, {"r", p.radius}}; break;
            case Primitive::Kind::Arc:
                item = {{"k", "arc"}, {"a", pointToJson(p.a)}, {"r", p.radius}, {"from", p.startAngle}, {"to", p.endAngle}};
                break;
            case Primitive::Kind::Text: item = {{"k", "text"}, {"a", pointToJson(p.a)}, {"text", p.text}, {"h", p.textHeight}}; break;
            case Primitive::Kind::Dot: item = {{"k", "dot"}, {"a", pointToJson(p.a)}, {"r", p.radius}}; break;
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Primitive> primitivesFromJson(const json& list) {
    std::vector<Primitive> out;
    for (const auto& item : list) {
        Primitive p;
        const std::string kind = item.at("k").get<std::string>();
        p.a = pointFromJson(item.at("a"));
        if (kind == "line") {
            p.kind = Primitive::Kind::Line;
            p.b = pointFromJson(item.at("b"));
        } else if (kind == "circle" || kind == "dot") {
            p.kind = kind == "dot" ? Primitive::Kind::Dot : Primitive::Kind::Circle;
            p.radius = item.at("r").get<double>();
        } else if (kind == "arc") {
            p.kind = Primitive::Kind::Arc;
            p.radius = item.at("r").get<double>();
            p.startAngle = item.at("from").get<double>();
            p.endAngle = item.at("to").get<double>();
        } else if (kind == "text") {
            p.kind = Primitive::Kind::Text;
            p.text = item.at("text").get<std::string>();
            p.textHeight = item.value("h", 3.5);
        } else {
            throw json::other_error::create(0, "неизвестный вид графики «" + kind + "»", &item);
        }
        out.push_back(std::move(p));
    }
    return out;
}

void expandBy(BBox2& box, const Primitive& p) {
    if (p.kind == Primitive::Kind::Line) {
        box.expand(p.a);
        box.expand(p.b);
    } else if (p.kind == Primitive::Kind::Text) {
        const double half = 0.3 * p.textHeight * static_cast<double>(characterCount(p.text));
        box.expand(Point2{p.a.x - half, p.a.y});
        box.expand(Point2{p.a.x + half, p.a.y + p.textHeight});
    } else if (p.kind == Primitive::Kind::Arc) {
        box.expand(geometry::arcBounds(p.a, p.radius, p.startAngle, p.endAngle));
    } else {
        box.expand(Point2{p.a.x - p.radius, p.a.y - p.radius});
        box.expand(Point2{p.a.x + p.radius, p.a.y + p.radius});
    }
}

}  // namespace

void BlockDefinition::writeJson(json& j) const { j["content"] = primitivesToJson(content); }

Result<void> BlockDefinition::readJson(const json& j) {
    return guarded("блок", [&] { content = primitivesFromJson(j.at("content")); });
}

std::vector<Primitive> BlockReference::primitives() const {
    std::vector<Primitive> out;
    out.reserve(content.size());
    const double c = std::cos(angle) * scale, s = std::sin(angle) * scale;
    const auto place = [&](Point2 p) { return Point2{position.x + p.x * c - p.y * s, position.y + p.x * s + p.y * c}; };
    for (Primitive p : content) {
        p.a = place(p.a);
        p.b = place(p.b);
        p.radius *= scale;
        p.textHeight *= scale;
        if (p.kind == Primitive::Kind::Arc) {
            p.startAngle = geometry::normalizeAngle(p.startAngle + angle);
            p.endAngle = geometry::normalizeAngle(p.endAngle + angle);
        }
        out.push_back(std::move(p));
    }
    return out;
}

BBox2 BlockReference::bounds() const {
    BBox2 box;
    box.expand(position);
    for (const auto& p : primitives()) expandBy(box, p);
    return box;
}

std::vector<Point2> BlockReference::snapPoints() const {
    std::vector<Point2> out{position};
    for (const auto& p : primitives())
        if (p.kind == Primitive::Kind::Line) {
            out.push_back(p.a);
            out.push_back(p.b);
        } else if (p.kind == Primitive::Kind::Circle || p.kind == Primitive::Kind::Arc) {
            out.push_back(p.a);
        }
    return out;
}

void BlockReference::transform(const geometry::Transform2& t) {
    position = t.apply(position);
    if (!t.mirrored) angle = geometry::normalizeAngle(angle + t.angle());  // отражение блока не поддерживается: он только переносится
    scale *= t.scale();
}

void BlockReference::writeJson(json& j) const {
    writeEntityJson(j);
    j["definition"] = definitionId.toString();
    j["position"] = pointToJson(position);
    j["angle"] = angle;
    j["scale"] = scale;
    j["content"] = primitivesToJson(content);
}

Result<void> BlockReference::readJson(const json& j) {
    bool badId = false;
    auto parsed = guarded("вставка блока", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        auto id = Uuid::parse(j.at("definition").get<std::string>());
        if (id)
            definitionId = id.value();
        else
            badId = true;
        position = pointFromJson(j.at("position"));
        angle = j.value("angle", 0.0);
        scale = j.value("scale", 1.0);
        content = primitivesFromJson(j.at("content"));
    });
    if (!parsed) return parsed;
    if (badId) return makeError(ErrorCode::ParseError, "вставка блока: неверный номер описания блока");
    return {};
}

// ---- RoughnessSign ----

namespace {

// Черты знака с вершиной в точке tip. Наклон черт к линии поверхности — 60°.
void appendSignStrokes(std::vector<Primitive>& out, Point2 tip, double h, double bigH, int quarterTurns,
                       RoughnessSign::Kind kind) {
    const double slope = 1.0 / std::tan(std::numbers::pi / 3.0);  // сдвиг по горизонтали на единицу высоты
    const auto place = [&](double x, double y) {
        for (int i = 0; i < ((quarterTurns % 4) + 4) % 4; ++i) {
            const double nx = -y, ny = x;
            x = nx;
            y = ny;
        }
        return Point2{tip.x + x, tip.y + y};
    };
    const auto line = [&](double x1, double y1, double x2, double y2) {
        Primitive p;
        p.kind = Primitive::Kind::Line;
        p.a = place(x1, y1);
        p.b = place(x2, y2);
        out.push_back(p);
    };
    line(0, 0, -h * slope, h);
    line(0, 0, bigH * slope, bigH);
    if (kind == RoughnessSign::Kind::Removal) line(-h * slope, h, h * slope, h);
    if (kind == RoughnessSign::Kind::NoRemoval) {
        // Окружность, вписанная между чертами и уровнем h.
        Primitive c;
        c.kind = Primitive::Kind::Circle;
        c.a = place(0, 2.0 * h / 3.0);
        c.radius = h / 3.0;
        out.push_back(c);
    }
}

double signTextWidth(const std::string& text, double height) {
    return static_cast<double>(characterCount(text)) * height * 0.62;
}

}  // namespace

std::vector<Primitive> RoughnessSign::primitives() const {
    std::vector<Primitive> out;
    const double h = textHeight;
    const bool shelf = !parameter.empty() || !method.empty();
    // С полкой длинная черта выше: под полкой помещается строка высотой h.
    const double bigH = shelf ? 2.4 * h : 2.0 * h;
    appendSignStrokes(out, position, h, bigH, shelf ? 0 : quarterTurns, kind);
    const double slope = 1.0 / std::tan(std::numbers::pi / 3.0);
    double right = position.x + bigH * slope;
    if (shelf) {
        const double width = std::max(signTextWidth(parameter, h), signTextWidth(method, h)) + 0.6 * h;
        Primitive line;
        line.kind = Primitive::Kind::Line;
        line.a = {position.x + bigH * slope, position.y + bigH};
        line.b = {line.a.x + width, line.a.y};
        out.push_back(line);
        const auto text = [&](const std::string& value, double y) {
            if (value.empty()) return;
            Primitive t;
            t.kind = Primitive::Kind::Text;
            t.text = value;
            t.textHeight = h;
            t.a = {line.a.x + width / 2.0, y};
            out.push_back(t);
        };
        text(parameter, position.y + bigH - h - 0.25 * h);
        text(method, position.y + bigH + 0.25 * h);
        right = line.b.x;
    }
    if (others) {
        // «( √ )»: знак тех же размеров, что на изображении, в скобках.
        const double g = othersHeight;
        const double left = right + 0.8 * g;
        const auto bracket = [&](const char* glyph, double x) {
            Primitive t;
            t.kind = Primitive::Kind::Text;
            t.text = glyph;
            t.textHeight = g * 1.4;
            t.a = {x, position.y};
            out.push_back(t);
        };
        bracket("(", left);
        appendSignStrokes(out, {left + 1.6 * g, position.y}, g, 2.0 * g, 0, Kind::Any);
        bracket(")", left + 1.6 * g + 2.0 * g * slope + 0.8 * g);
    }
    return out;
}

BBox2 RoughnessSign::bounds() const {
    BBox2 box;
    box.expand(position);
    for (const auto& p : primitives()) {
        if (p.kind == Primitive::Kind::Line) {
            box.expand(p.a);
            box.expand(p.b);
        } else if (p.kind == Primitive::Kind::Circle) {
            box.expand(Point2{p.a.x - p.radius, p.a.y - p.radius});
            box.expand(Point2{p.a.x + p.radius, p.a.y + p.radius});
        } else if (p.kind == Primitive::Kind::Text) {
            const double half = signTextWidth(p.text, p.textHeight) / 2.0;
            box.expand(Point2{p.a.x - half, p.a.y});
            box.expand(Point2{p.a.x + half, p.a.y + p.textHeight});
        }
    }
    return box;
}

void RoughnessSign::transform(const geometry::Transform2& t) {
    position = t.apply(position);
    if (!t.mirrored) quarterTurns = (quarterTurns + static_cast<int>(std::lround(t.angle() / (std::numbers::pi / 2.0))) % 4 + 4) % 4;
}

void RoughnessSign::writeJson(json& j) const {
    writeEntityJson(j);
    j["position"] = pointToJson(position);
    j["kind"] = kind == Kind::Removal ? "removal" : kind == Kind::NoRemoval ? "noRemoval" : "any";
    j["quarterTurns"] = quarterTurns;
    j["parameter"] = parameter;
    if (!method.empty()) j["method"] = method;
    j["textHeight"] = textHeight;
    if (others) {
        j["others"] = true;
        j["othersHeight"] = othersHeight;
    }
}

Result<void> RoughnessSign::readJson(const json& j) {
    return guarded("знак шероховатости", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        position = pointFromJson(j.at("position"));
        const std::string name = j.value("kind", std::string("any"));
        kind = name == "removal" ? Kind::Removal : name == "noRemoval" ? Kind::NoRemoval : Kind::Any;
        quarterTurns = j.value("quarterTurns", 0);
        parameter = j.value("parameter", std::string());
        method = j.value("method", std::string());
        textHeight = j.value("textHeight", 3.5);
        others = j.value("others", false);
        othersHeight = j.value("othersHeight", 3.5);
    });
}

// ---- RadialDimension ----

BBox2 RadialDimension::bounds() const {
    Point2 tip = geometry::pointOnCircle(center, radius, angle);
    BBox2 box = BBox2::fromCorners(center, tip);
    if (diameter) box.expand(geometry::pointOnCircle(center, radius, angle + std::numbers::pi));
    box.expand(geometry::pointOnCircle(center, radius + outsideExtension(), angle));
    return box;
}

void RadialDimension::transform(const geometry::Transform2& t) {
    center = t.apply(center);
    radius *= t.scale();
    angle = geometry::normalizeAngle(t.mirrored ? t.angle() - angle : angle + t.angle());
}

void RadialDimension::writeJson(json& j) const {
    writeEntityJson(j);
    j["center"] = pointToJson(center);
    j["radius"] = radius;
    j["angle"] = angle;
    j["diameter"] = diameter;
    j["textHeight"] = textHeight;
    note.write(j);
}

Result<void> RadialDimension::readJson(const json& j) {
    return guarded("размер радиуса", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        center = pointFromJson(j.at("center"));
        radius = j.at("radius").get<double>();
        angle = j.value("angle", 0.7853981633974483);
        diameter = j.value("diameter", false);
        textHeight = j.value("textHeight", 3.5);
        note.read(j);
    });
}

// ---- AngularDimension ----

double AngularDimension::valueDegrees() const {
    double a1 = std::atan2(p1.y - vertex.y, p1.x - vertex.x);
    double a2 = std::atan2(p2.y - vertex.y, p2.x - vertex.x);
    return geometry::normalizeAngle(a2 - a1) * 180.0 / std::numbers::pi;
}

BBox2 AngularDimension::bounds() const {
    double a1 = std::atan2(p1.y - vertex.y, p1.x - vertex.x);
    double a2 = std::atan2(p2.y - vertex.y, p2.x - vertex.x);
    BBox2 box = geometry::arcBounds(vertex, arcRadius, a1, a2);
    box.expand(p1);
    box.expand(p2);
    return box;
}

void AngularDimension::transform(const geometry::Transform2& t) {
    vertex = t.apply(vertex);
    p1 = t.apply(p1);
    p2 = t.apply(p2);
    arcRadius *= t.scale();
    if (t.mirrored) std::swap(p1, p2);  // отражение меняет направление обхода; измеряемый угол тот же
}

void AngularDimension::writeJson(json& j) const {
    writeEntityJson(j);
    j["vertex"] = pointToJson(vertex);
    j["p1"] = pointToJson(p1);
    j["p2"] = pointToJson(p2);
    j["arcRadius"] = arcRadius;
    j["textHeight"] = textHeight;
}

Result<void> AngularDimension::readJson(const json& j) {
    return guarded("угловой размер", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        vertex = pointFromJson(j.at("vertex"));
        p1 = pointFromJson(j.at("p1"));
        p2 = pointFromJson(j.at("p2"));
        arcRadius = j.at("arcRadius").get<double>();
        textHeight = j.value("textHeight", 3.5);
    });
}

// ---- Hatch ----

std::vector<std::pair<Point2, Point2>> Hatch::strokes() const {
    std::vector<std::pair<Point2, Point2>> out;
    if (!(spacing > 1e-3) || !std::isfinite(angle)) return out;

    // Поворачиваем контуры так, чтобы линии штриховки стали горизонтальными.
    const double c = std::cos(-angle), s = std::sin(-angle);
    auto rotate = [&](Point2 p) { return Point2{c * p.x - s * p.y, s * p.x + c * p.y}; };
    auto rotateBack = [&](Point2 p) { return Point2{c * p.x + s * p.y, -s * p.x + c * p.y}; };

    struct Edge {
        Point2 a, b;
    };
    std::vector<Edge> edges;
    double yMin = 1e300, yMax = -1e300;
    for (const auto& loop : loops) {
        if (loop.size() < 3) continue;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            Edge e{rotate(loop[i]), rotate(loop[(i + 1) % loop.size()])};
            yMin = std::min({yMin, e.a.y, e.b.y});
            yMax = std::max({yMax, e.a.y, e.b.y});
            edges.push_back(e);
        }
    }
    if (edges.empty()) return out;
    if ((yMax - yMin) / spacing > 200000.0) return out;  // защита от недопустимо мелкого шага

    // Линии привязаны к началу координат, поэтому соседние штриховки совпадают по шагу.
    // Малый сдвиг уводит линию от вершин контура, лежащих точно на шаге.
    const double nudge = spacing * 1e-6;
    std::vector<double> xs;
    // Сдвиг на полшага: линия штриховки не ложится на сторону контура, проходящую через начало шага.
    for (double k = std::floor(yMin / spacing); (k + 0.5) * spacing <= yMax; k += 1.0) {
        double y = (k + 0.5) * spacing + nudge;
        if (y < yMin) continue;
        xs.clear();
        for (const Edge& e : edges)
            if ((e.a.y <= y) != (e.b.y <= y)) xs.push_back(e.a.x + (y - e.a.y) * (e.b.x - e.a.x) / (e.b.y - e.a.y));
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
            if (xs[i + 1] - xs[i] > 1e-9) out.emplace_back(rotateBack({xs[i], y}), rotateBack({xs[i + 1], y}));
    }
    return out;
}

BBox2 Hatch::bounds() const {
    BBox2 box;
    for (const auto& loop : loops)
        for (Point2 p : loop) box.expand(p);
    return box;
}

void Hatch::transform(const geometry::Transform2& t) {
    for (auto& loop : loops)
        for (Point2& p : loop) p = t.apply(p);
    spacing *= t.scale();
    angle = t.mirrored ? t.angle() - angle : angle + t.angle();
}

void Hatch::writeJson(json& j) const {
    writeEntityJson(j);
    json loopsJson = json::array();
    for (const auto& loop : loops) {
        json points = json::array();
        for (Point2 p : loop) points.push_back(pointToJson(p));
        loopsJson.push_back(std::move(points));
    }
    j["loops"] = std::move(loopsJson);
    j["angle"] = angle;
    j["spacing"] = spacing;
}

Result<void> Hatch::readJson(const json& j) {
    return guarded("штриховка", [&] {
        if (auto r = readEntityJson(j); !r) throw json::other_error::create(0, r.error().message, &j);
        loops.clear();
        for (const auto& loopJson : j.at("loops")) {
            std::vector<Point2> loop;
            for (const auto& p : loopJson) loop.push_back(pointFromJson(p));
            loops.push_back(std::move(loop));
        }
        angle = j.value("angle", 0.7853981633974483);
        spacing = j.at("spacing").get<double>();
    });
}

// ---- ModelView ----

void ModelView::writeJson(json& j) const {
    j["source"] = sourceDocumentId.toString();
    j["view"] = view;
    j["origin"] = pointToJson(origin);
    j["scale"] = scale;
    j["showHidden"] = showHidden;
    if (section) {
        j["section"] = true;
        j["sectionAt"] = sectionAt;
        if (sectionOnly) j["sectionOnly"] = true;
    }
    if (!stamp.empty()) j["stamp"] = stamp;
    if (exploded) j["exploded"] = true;
    if (!label.empty()) j["label"] = label;
    json list = json::array();
    for (const auto& id : entities) list.push_back(id.toString());
    j["entities"] = std::move(list);
    if (!marks.empty()) {
        json drawn = json::array();
        for (const auto& id : marks) drawn.push_back(id.toString());
        j["marks"] = std::move(drawn);
    }
    if (!dimensions.empty()) {
        json attached = json::array();
        for (const auto& id : dimensions) attached.push_back(id.toString());
        j["dimensions"] = std::move(attached);
    }
}

Result<void> ModelView::readJson(const json& j) {
    bool badId = false;
    auto parsed = guarded("вид модели", [&] {
        auto source = Uuid::parse(j.at("source").get<std::string>());
        if (source)
            sourceDocumentId = source.value();
        else
            badId = true;
        view = j.value("view", std::string("front"));
        origin = pointFromJson(j.at("origin"));
        scale = j.value("scale", 1.0);
        showHidden = j.value("showHidden", false);
        section = j.value("section", false);
        sectionAt = j.value("sectionAt", 0.0);
        sectionOnly = j.value("sectionOnly", false);
        stamp = j.value("stamp", std::string());
        exploded = j.value("exploded", false);
        label = j.value("label", std::string());
        entities.clear();
        for (const auto& item : j.at("entities")) {
            auto id = Uuid::parse(item.get<std::string>());
            if (id)
                entities.push_back(id.value());
            else
                badId = true;
        }
        marks.clear();
        if (j.contains("marks"))
            for (const auto& item : j.at("marks")) {
                auto id = Uuid::parse(item.get<std::string>());
                if (id)
                    marks.push_back(id.value());
                else
                    badId = true;
            }
        dimensions.clear();
        if (j.contains("dimensions"))
            for (const auto& item : j.at("dimensions")) {
                auto id = Uuid::parse(item.get<std::string>());
                if (id)
                    dimensions.push_back(id.value());
                else
                    badId = true;
            }
    });
    if (!parsed) return parsed;
    if (badId) return makeError(ErrorCode::ParseError, "вид модели: неверный номер объекта");
    return {};
}

void registerTypes(document::ObjectFactory& factory) {
    factory.registerType<Layer>();
    factory.registerType<Sheet>();
    factory.registerType<LineEntity>();
    factory.registerType<CircleEntity>();
    factory.registerType<ArcEntity>();
    factory.registerType<TextEntity>();
    factory.registerType<LinearDimension>();
    factory.registerType<RadialDimension>();
    factory.registerType<AngularDimension>();
    factory.registerType<Hatch>();
    factory.registerType<Leader>();
    factory.registerType<RoughnessSign>();
    factory.registerType<BlockDefinition>();
    factory.registerType<BlockReference>();
    factory.registerType<ModelView>();
}

}  // namespace cosmocad::drawing
