#include "cosmocad/modeling/ModelingService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/modeling/Expression.h"

namespace cosmocad::modeling {

using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Uuid;
using document::Document;
using document::Transaction;
using geometry::Plane;
using geometry::Point2;
using geometry::Point3;
using geometry::Profile2D;
using geometry::ProfileSegment;
using geometry::ShapePtr;
using geometry::Vector3;

namespace {

using Values = std::map<std::string, double>;

Result<void> requireKind(const Document& part) {
    if (part.kind() != document::DocumentKind::Part)
        return makeError(ErrorCode::InvalidArgument, "операция применима только к документу «Деталь»");
    return {};
}

Result<double> eval(const std::string& expression, const Values& values) {
    return evaluateExpression(expression, [&](std::string_view name) -> std::optional<double> {
        const auto it = values.find(std::string(name));
        if (it == values.end()) return std::nullopt;
        return it->second;
    });
}

const Parameter* findParameter(const Document& part, const std::string& name) {
    for (const auto* p : part.objectsOf<Parameter>())
        if (p->name == name) return p;
    return nullptr;
}

std::string number(double v) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.12g", v == 0.0 ? 0.0 : v);
    return buffer;
}

std::string fingerprint(const std::string& text) {
    // Два независимых прохода FNV-1a: 128 бит хватает, чтобы совпадения были невозможны на практике.
    std::uint64_t a = 1469598103934665603ULL, b = 0x9E3779B97F4A7C15ULL;
    for (const unsigned char c : text) {
        a = (a ^ c) * 1099511628211ULL;
        b = (b ^ c) * 0x100000001B3ULL + 0x632BE59BD9B4E019ULL;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return buffer;
}

// ---- Эскиз → контуры ----

struct ResolvedShape {
    SketchShape::Kind kind;
    std::uint64_t tag;
    bool hole;
    std::vector<double> v;
};

Result<std::vector<ResolvedShape>> resolveShapes(const Sketch& sketch, const Values& values) {
    std::vector<ResolvedShape> out;
    for (const auto& shape : sketch.shapes) {
        ResolvedShape r{shape.kind, shape.tag, shape.hole, {}};
        for (const auto& text : shape.values) {
            auto value = eval(text, values);
            if (!value) return value.error();
            r.v.push_back(value.value());
        }
        switch (shape.kind) {
            case SketchShape::Kind::Rectangle:
                if (r.v.size() != 4) return makeError(ErrorCode::InvalidArgument, "у прямоугольника эскиза должно быть 4 размера");
                if (r.v[2] <= 0.0 || r.v[3] <= 0.0)
                    return makeError(ErrorCode::InvalidArgument, "ширина и высота прямоугольника эскиза должны быть больше нуля");
                break;
            case SketchShape::Kind::Circle:
                if (r.v.size() != 3) return makeError(ErrorCode::InvalidArgument, "у окружности эскиза должно быть 3 размера");
                if (r.v[2] <= 0.0) return makeError(ErrorCode::InvalidArgument, "радиус окружности эскиза должен быть больше нуля");
                break;
            case SketchShape::Kind::Polygon:
                if (r.v.size() < 6 || r.v.size() % 2 != 0)
                    return makeError(ErrorCode::InvalidArgument, "у многоугольника эскиза должно быть не меньше трёх точек");
                break;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Point2> cornersOf(const ResolvedShape& s) {
    std::vector<Point2> points;
    if (s.kind == SketchShape::Kind::Rectangle) {
        points = {{s.v[0], s.v[1]}, {s.v[0] + s.v[2], s.v[1]}, {s.v[0] + s.v[2], s.v[1] + s.v[3]}, {s.v[0], s.v[1] + s.v[3]}};
    } else if (s.kind == SketchShape::Kind::Polygon) {
        for (std::size_t i = 0; i + 1 < s.v.size(); i += 2) points.push_back({s.v[i], s.v[i + 1]});
    }
    return points;
}

std::vector<ProfileSegment> loopOf(const ResolvedShape& s) {
    std::vector<ProfileSegment> loop;
    if (s.kind == SketchShape::Kind::Circle) {
        ProfileSegment seg;
        seg.kind = ProfileSegment::Kind::Circle;
        seg.center = {s.v[0], s.v[1]};
        seg.start = {s.v[0] + s.v[2], s.v[1]};
        seg.sourceTag = s.tag * 1000;
        loop.push_back(seg);
        return loop;
    }
    const auto points = cornersOf(s);
    for (std::size_t i = 0; i < points.size(); ++i) {
        ProfileSegment seg;
        seg.start = points[i];
        seg.end = points[(i + 1) % points.size()];
        seg.sourceTag = s.tag * 1000 + i;
        loop.push_back(seg);
    }
    return loop;
}

Point2 referencePoint(const ResolvedShape& s) {
    if (s.kind == SketchShape::Kind::Circle) return {s.v[0], s.v[1]};
    const auto points = cornersOf(s);
    Point2 sum{0, 0};
    for (const auto& p : points) sum = sum + p;
    return sum * (1.0 / static_cast<double>(points.size()));
}

bool contains(const ResolvedShape& s, Point2 p) {
    if (s.kind == SketchShape::Kind::Circle) return geometry::distance(p, {s.v[0], s.v[1]}) < s.v[2];
    const auto points = cornersOf(s);
    bool inside = false;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const Point2 a = points[i], b = points[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

Result<std::vector<Profile2D>> profilesOf(const std::vector<ResolvedShape>& shapes) {
    std::vector<Profile2D> profiles;
    std::vector<const ResolvedShape*> outers;
    for (const auto& s : shapes)
        if (!s.hole) {
            Profile2D profile;
            profile.outer = loopOf(s);
            profiles.push_back(std::move(profile));
            outers.push_back(&s);
        }
    if (profiles.empty()) return makeError(ErrorCode::InvalidArgument, "в эскизе нет ни одной фигуры");
    for (const auto& s : shapes) {
        if (!s.hole) continue;
        bool placed = false;
        for (std::size_t i = 0; i < outers.size() && !placed; ++i)
            if (contains(*outers[i], referencePoint(s))) {
                profiles[i].holes.push_back(loopOf(s));
                placed = true;
            }
        if (!placed) return makeError(ErrorCode::InvalidArgument, "отверстие эскиза лежит вне его фигур");
    }
    return profiles;
}

Result<Plane> planeOf(const Sketch& sketch, const Values& values, geometry::IGeometryKernel& kernel,
                      const ShapePtr& body) {
    Plane plane;
    if (!sketch.faceName.empty()) {
        if (!body) return makeError(ErrorCode::InvalidState, "эскиз лежит на грани, но тела ещё нет");
        auto info = kernel.faceInfo(body, sketch.faceName);
        if (!info)
            return makeError(ErrorCode::NotFound, "грань «" + sketch.faceName + "», на которой лежит эскиз, не найдена");
        if (info.value().kind != geometry::FaceInfo::Kind::Plane)
            return makeError(ErrorCode::InvalidArgument, "эскиз можно положить только на плоскую грань");
        const Vector3 n = geometry::normalized(info.value().direction);
        const Point3 p = info.value().point;
        const double d = n.x * p.x + n.y * p.y + n.z * p.z;
        plane.origin = {n.x * d, n.y * d, n.z * d};  // ближайшая к началу координат точка грани
        plane.normal = n;
        const Vector3 seed = std::abs(n.x) < 0.9 ? Vector3{1, 0, 0} : Vector3{0, 1, 0};
        const double k = geometry::dotProduct(seed, n);
        plane.xDirection = geometry::normalized(Vector3{seed.x - n.x * k, seed.y - n.y * k, seed.z - n.z * k});
    } else if (sketch.plane == "XY") {
        plane.normal = {0, 0, 1};
        plane.xDirection = {1, 0, 0};
    } else if (sketch.plane == "XZ") {
        plane.normal = {0, -1, 0};
        plane.xDirection = {1, 0, 0};
    } else if (sketch.plane == "YZ") {
        plane.normal = {1, 0, 0};
        plane.xDirection = {0, 1, 0};
    } else {
        return makeError(ErrorCode::InvalidArgument, "неизвестная плоскость эскиза «" + sketch.plane + "»");
    }
    auto offset = eval(sketch.offset, values);
    if (!offset) return offset.error();
    plane.origin = {plane.origin.x + plane.normal.x * offset.value(), plane.origin.y + plane.normal.y * offset.value(),
                    plane.origin.z + plane.normal.z * offset.value()};
    return plane;
}

std::string describe(const Plane& p) {
    std::string s;
    for (const double v : {p.origin.x, p.origin.y, p.origin.z, p.normal.x, p.normal.y, p.normal.z, p.xDirection.x,
                           p.xDirection.y, p.xDirection.z})
        s += number(v) + ",";
    return s;
}

std::string describe(const std::vector<ResolvedShape>& shapes) {
    std::string s;
    for (const auto& shape : shapes) {
        s += std::to_string(static_cast<int>(shape.kind)) + ":" + std::to_string(shape.tag) + (shape.hole ? "h" : "") + "(";
        for (const double v : shape.v) s += number(v) + ",";
        s += ")";
    }
    return s;
}

// Проверка формул при вводе: формула должна читаться, а её имена — существовать.
Result<void> checkExpression(const Document& part, const std::string& expression, const Values& values) {
    (void)part;
    auto value = eval(expression, values);
    if (!value) return value.error();
    return {};
}

}  // namespace

// ---- Параметры ----

Result<Values> ModelingService::parameterValues(const Document& part) const {
    Values done;
    std::map<std::string, const Parameter*> all;
    for (const auto* p : part.objectsOf<Parameter>()) all[p->name] = p;
    std::vector<std::string> stack;
    std::optional<core::Error> problem;

    std::function<std::optional<double>(const std::string&)> resolve = [&](const std::string& name) -> std::optional<double> {
        if (const auto it = done.find(name); it != done.end()) return it->second;
        const auto it = all.find(name);
        if (it == all.end()) return std::nullopt;
        if (std::find(stack.begin(), stack.end(), name) != stack.end()) {
            std::string chain;
            for (const auto& s : stack) chain += s + " → ";
            problem = makeError(ErrorCode::InvalidState, "параметры ссылаются друг на друга по кругу: " + chain + name);
            return std::nullopt;
        }
        stack.push_back(name);
        auto value = evaluateExpression(it->second->expression, [&](std::string_view inner) { return resolve(std::string(inner)); });
        stack.pop_back();
        if (!value) {
            if (!problem)
                problem = makeError(value.error().code, "параметр «" + name + "»: " + value.error().message);
            return std::nullopt;
        }
        done[name] = value.value();
        return value.value();
    };

    for (const auto& [name, parameter] : all) {
        (void)parameter;
        resolve(name);
        if (problem) return *problem;
    }
    return done;
}

Result<Uuid> ModelingService::addParameter(Document& part, const std::string& name, const std::string& expression) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (!isValidParameterName(name))
        return makeError(ErrorCode::InvalidArgument,
                         "имя параметра «" + name + "» не годится: нужны буквы, цифры или «_», не с цифры, "
                         "и не имя функции или единицы измерения");
    if (findParameter(part, name)) return makeError(ErrorCode::AlreadyExists, "параметр «" + name + "» уже есть");
    Transaction tx(part, "Новый параметр");
    auto parameter = std::make_unique<Parameter>();
    parameter->name = name;
    parameter->expression = expression;
    auto id = tx.add(std::move(parameter));
    if (!id) return id;
    if (auto values = parameterValues(part); !values) {
        tx.rollback();
        return values.error();
    }
    tx.commit();
    return id;
}

Result<void> ModelingService::setParameter(Document& part, const std::string& name, const std::string& expression) {
    if (auto ok = requireKind(part); !ok) return ok;
    const Parameter* parameter = findParameter(part, name);
    if (!parameter) return makeError(ErrorCode::NotFound, "параметр «" + name + "» не найден");
    Transaction tx(part, "Изменение параметра");
    if (auto ok = tx.modifyAs<Parameter>(parameter->id(), [&](Parameter& p) { p.expression = expression; }); !ok) return ok;
    if (auto values = parameterValues(part); !values) {
        tx.rollback();
        return values.error();
    }
    tx.commit();
    return {};
}

Result<void> ModelingService::removeParameter(Document& part, const std::string& name) {
    if (auto ok = requireKind(part); !ok) return ok;
    const Parameter* parameter = findParameter(part, name);
    if (!parameter) return makeError(ErrorCode::NotFound, "параметр «" + name + "» не найден");
    const auto uses = [&](const std::string& expression) {
        auto names = expressionNames(expression);
        return names && std::find(names.value().begin(), names.value().end(), name) != names.value().end();
    };
    std::string user;
    for (const auto* p : part.objectsOf<Parameter>())
        if (p != parameter && uses(p->expression)) user = "параметре «" + p->name + "»";
    for (const auto* s : part.objectsOf<Sketch>()) {
        if (uses(s->offset)) user = "эскизе «" + s->name + "»";
        for (const auto& shape : s->shapes)
            for (const auto& v : shape.values)
                if (uses(v)) user = "эскизе «" + s->name + "»";
    }
    for (const auto* f : part.objectsOf<Feature>())
        if (uses(f->value) || uses(f->pattern.count) || uses(f->pattern.stepX) || uses(f->pattern.stepY) ||
            uses(f->pattern.centerX) || uses(f->pattern.centerY) || uses(f->pattern.angle))
            user = "операции «" + f->name + "»";
    if (!user.empty())
        return makeError(ErrorCode::InvalidState, "параметр «" + name + "» нельзя удалить: он используется в " + user);
    Transaction tx(part, "Удаление параметра");
    if (auto ok = tx.remove(parameter->id()); !ok) return ok;
    tx.commit();
    return {};
}

// ---- Эскизы ----

namespace {

Result<void> checkShapes(const Document& part, const std::vector<SketchShape>& shapes, const Values& values) {
    Sketch probe;
    probe.shapes = shapes;
    auto resolved = resolveShapes(probe, values);
    if (!resolved) return resolved.error();
    auto profiles = profilesOf(resolved.value());
    if (!profiles) return profiles.error();
    (void)part;
    return {};
}

}  // namespace

Result<Uuid> ModelingService::addSketch(Document& part, const SketchParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkShapes(part, params.shapes, values.value()); !ok) return ok.error();
    if (auto ok = checkExpression(part, params.offset, values.value()); !ok) return ok.error();
    if (params.faceName.empty() && params.plane != "XY" && params.plane != "XZ" && params.plane != "YZ")
        return makeError(ErrorCode::InvalidArgument, "неизвестная плоскость эскиза «" + params.plane + "»");
    auto sketch = std::make_unique<Sketch>();
    sketch->name = params.name.empty() ? "Эскиз " + std::to_string(part.objectsOf<Sketch>().size() + 1) : params.name;
    sketch->plane = params.faceName.empty() ? params.plane : std::string();
    sketch->offset = params.offset;
    sketch->faceName = params.faceName;
    sketch->shapes = params.shapes;
    sketch->sourceDrawingId = params.sourceDrawingId;
    sketch->sourceEntities = params.sourceEntities;
    for (auto& shape : sketch->shapes) shape.tag = sketch->nextTag++;
    Transaction tx(part, "Новый эскиз");
    auto id = tx.add(std::move(sketch));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<void> ModelingService::updateSketch(Document& part, const Uuid& sketchId, const std::vector<SketchShape>& shapes) {
    if (auto ok = requireKind(part); !ok) return ok;
    const auto* sketch = part.findAs<Sketch>(sketchId);
    if (!sketch) return makeError(ErrorCode::NotFound, "эскиз не найден");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkShapes(part, shapes, values.value()); !ok) return ok;
    Transaction tx(part, "Изменение эскиза");
    auto ok = tx.modifyAs<Sketch>(sketchId, [&](Sketch& s) {
        std::set<std::uint64_t> used;
        s.shapes = shapes;
        for (auto& shape : s.shapes) {
            // Прежняя метка сохраняется; новая фигура (метка 0 или повтор) получает следующий номер.
            if (shape.tag == 0 || shape.tag >= s.nextTag || !used.insert(shape.tag).second) {
                shape.tag = s.nextTag++;
                used.insert(shape.tag);
            }
        }
    });
    if (!ok) return ok;
    tx.commit();
    return {};
}

// ---- Операции ----

std::vector<const Feature*> ModelingService::features(const Document& part) const {
    auto list = part.objectsOf<Feature>();
    std::stable_sort(list.begin(), list.end(), [](const Feature* a, const Feature* b) { return a->sequence < b->sequence; });
    return list;
}

Result<Uuid> ModelingService::addFeature(Document& part, Feature feature, const char* name, const char* prefix) {
    int maxNumber = 0;
    std::int64_t maxSequence = 0;
    const std::string start(prefix);
    for (const auto* f : part.objectsOf<Feature>()) {
        maxSequence = std::max(maxSequence, f->sequence);
        if (f->tag.rfind(start, 0) == 0) maxNumber = std::max(maxNumber, std::atoi(f->tag.c_str() + start.size()));
    }
    feature.tag = start + std::to_string(maxNumber + 1);
    feature.sequence = maxSequence + 1;
    feature.name = std::string(name) + " " + std::to_string(maxNumber + 1);
    Transaction tx(part, name);
    auto id = tx.add(std::make_unique<Feature>(std::move(feature)));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<Uuid> ModelingService::addExtrude(Document& part, const ExtrudeParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (!part.findAs<Sketch>(params.sketchId)) return makeError(ErrorCode::NotFound, "эскиз для выдавливания не найден");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, params.distance, values.value()); !ok) return ok.error();
    Feature feature;
    feature.kind = FeatureKind::Extrude;
    feature.sketchId = params.sketchId;
    feature.value = params.distance;
    feature.cut = params.cut;
    feature.reversed = params.reversed;
    return addFeature(part, std::move(feature), params.cut ? "Вырез" : "Выдавливание", "E");
}

Result<Uuid> ModelingService::addRevolve(Document& part, const RevolveParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (!part.findAs<Sketch>(params.sketchId)) return makeError(ErrorCode::NotFound, "эскиз для вращения не найден");
    if (params.axis != "X" && params.axis != "Y")
        return makeError(ErrorCode::InvalidArgument, "ось вращения — X или Y эскиза");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, params.angle, values.value()); !ok) return ok.error();
    Feature feature;
    feature.kind = FeatureKind::Revolve;
    feature.sketchId = params.sketchId;
    feature.value = params.angle;
    feature.axis = params.axis;
    feature.cut = params.cut;
    return addFeature(part, std::move(feature), params.cut ? "Вырез вращением" : "Вращение", "R");
}

Result<Uuid> ModelingService::addBlend(Document& part, const BlendParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (params.edges.empty()) return makeError(ErrorCode::InvalidArgument, "не выбрано ни одного ребра");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, params.size, values.value()); !ok) return ok.error();
    Feature feature;
    feature.kind = params.chamfer ? FeatureKind::Chamfer : FeatureKind::Fillet;
    feature.value = params.size;
    feature.edges = params.edges;
    return addFeature(part, std::move(feature), params.chamfer ? "Фаска" : "Скругление", params.chamfer ? "C" : "F");
}

Result<Uuid> ModelingService::addSweep(Document& part, const SweepParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (!part.findAs<Sketch>(params.sketchId)) return makeError(ErrorCode::NotFound, "эскиз для протяжки не найден");
    if (params.path.empty()) return makeError(ErrorCode::InvalidArgument, "у траектории протяжки нет точек");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, params.bendRadius, values.value()); !ok) return ok.error();
    for (const auto& point : params.path)
        for (const auto& coordinate : point)
            if (auto ok = checkExpression(part, coordinate, values.value()); !ok) return ok.error();
    Feature feature;
    feature.kind = FeatureKind::Sweep;
    feature.sketchId = params.sketchId;
    feature.value = params.bendRadius;
    feature.path = params.path;
    feature.cut = params.cut;
    return addFeature(part, std::move(feature), params.cut ? "Вырез протяжкой" : "Протяжка", "S");
}

Result<Uuid> ModelingService::addMirror(Document& part, const MirrorParams& params) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    if (params.plane != "XY" && params.plane != "XZ" && params.plane != "YZ")
        return makeError(ErrorCode::InvalidArgument, "плоскость отражения должна быть XY, XZ или YZ, а указано «" + params.plane + "»");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, params.offset, values.value()); !ok) return ok.error();
    Feature feature;
    feature.kind = FeatureKind::Mirror;
    feature.value = params.offset;
    feature.axis = params.plane;
    return addFeature(part, std::move(feature), "Зеркало", "M");
}

Result<void> ModelingService::setFeatureValue(Document& part, const Uuid& featureId, const std::string& expression) {
    if (auto ok = requireKind(part); !ok) return ok;
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (auto ok = checkExpression(part, expression, values.value()); !ok) return ok;
    Transaction tx(part, "Изменение размера операции");
    if (auto ok = tx.modifyAs<Feature>(featureId, [&](Feature& f) { f.value = expression; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> ModelingService::setPattern(Document& part, const Uuid& featureId, const Pattern& pattern) {
    if (auto ok = requireKind(part); !ok) return ok;
    const auto* feature = part.findAs<Feature>(featureId);
    if (!feature) return makeError(ErrorCode::NotFound, "операция не найдена");
    if (feature->kind != FeatureKind::Extrude)
        return makeError(ErrorCode::InvalidArgument, "массив делается из выдавливания или выреза");
    auto values = parameterValues(part);
    if (!values) return values.error();
    if (pattern.kind != Pattern::Kind::None)
        for (const std::string* text : {&pattern.count, &pattern.stepX, &pattern.stepY, &pattern.centerX,
                                        &pattern.centerY, &pattern.angle})
            if (auto ok = checkExpression(part, *text, values.value()); !ok) return ok;
    Transaction tx(part, pattern.kind == Pattern::Kind::None ? "Отмена массива" : "Массив");
    if (auto ok = tx.modifyAs<Feature>(featureId, [&](Feature& f) { f.pattern = pattern; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> ModelingService::suppressFeature(Document& part, const Uuid& featureId, bool suppressed) {
    if (auto ok = requireKind(part); !ok) return ok;
    Transaction tx(part, suppressed ? "Отключение операции" : "Включение операции");
    if (auto ok = tx.modifyAs<Feature>(featureId, [&](Feature& f) { f.suppressed = suppressed; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> ModelingService::reorderFeature(Document& part, const Uuid& featureId, std::size_t newIndex) {
    if (auto ok = requireKind(part); !ok) return ok;
    auto list = features(part);
    const auto it = std::find_if(list.begin(), list.end(), [&](const Feature* f) { return f->id() == featureId; });
    if (it == list.end()) return makeError(ErrorCode::NotFound, "операция не найдена");
    if (newIndex >= list.size()) return makeError(ErrorCode::InvalidArgument, "нет такого места в дереве построения");
    const Feature* moved = *it;
    list.erase(it);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(newIndex), moved);
    Transaction tx(part, "Перестановка операции");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto sequence = static_cast<std::int64_t>(i + 1);
        if (list[i]->sequence == sequence) continue;
        if (auto ok = tx.modifyAs<Feature>(list[i]->id(), [&](Feature& f) { f.sequence = sequence; }); !ok) return ok;
    }
    tx.commit();
    return {};
}

Result<void> ModelingService::rollTo(Document& part, const Uuid& featureId) {
    if (auto ok = requireKind(part); !ok) return ok;
    if (!featureId.isNil() && !part.findAs<Feature>(featureId)) return makeError(ErrorCode::NotFound, "операция не найдена");
    Transaction tx(part, featureId.isNil() ? "Снять откат" : "Откат дерева построения");
    const auto existing = part.objectsOf<PartProperties>();
    if (existing.empty()) {
        auto properties = std::make_unique<PartProperties>();
        properties->rollbackAfter = featureId;
        if (auto id = tx.add(std::move(properties)); !id) return id.error();
    } else if (auto ok = tx.modifyAs<PartProperties>(existing.front()->id(), [&](PartProperties& p) { p.rollbackAfter = featureId; }); !ok) {
        return ok;
    }
    tx.commit();
    return {};
}

Result<void> ModelingService::removeFeature(Document& part, const Uuid& featureId) {
    if (auto ok = requireKind(part); !ok) return ok;
    const auto* feature = part.findAs<Feature>(featureId);
    if (!feature) return makeError(ErrorCode::NotFound, "операция не найдена");
    const Uuid sketchId = feature->sketchId;
    bool sketchShared = false;
    for (const auto* f : part.objectsOf<Feature>())
        if (f != feature && f->sketchId == sketchId) sketchShared = true;
    Transaction tx(part, "Удаление операции");
    if (auto ok = tx.remove(featureId); !ok) return ok;
    for (const auto* properties : part.objectsOf<PartProperties>())
        if (properties->rollbackAfter == featureId)
            if (auto ok = tx.modifyAs<PartProperties>(properties->id(), [](PartProperties& p) { p.rollbackAfter = Uuid(); }); !ok) return ok;
    if (!sketchId.isNil() && !sketchShared && part.findAs<Sketch>(sketchId))
        if (auto ok = tx.remove(sketchId); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> ModelingService::setPartProperties(Document& part, const std::string& designation,
                                                const std::string& material, double density) {
    if (auto ok = requireKind(part); !ok) return ok;
    if (!(density > 0.0)) return makeError(ErrorCode::InvalidArgument, "плотность должна быть больше нуля");
    Transaction tx(part, "Свойства детали");
    const auto existing = part.objectsOf<PartProperties>();
    if (existing.empty()) {
        auto properties = std::make_unique<PartProperties>();
        properties->designation = designation;
        properties->material = material;
        properties->density = density;
        if (auto id = tx.add(std::move(properties)); !id) return id.error();
    } else {
        auto ok = tx.modifyAs<PartProperties>(existing.front()->id(), [&](PartProperties& p) {
            p.designation = designation;
            p.material = material;
            p.density = density;
        });
        if (!ok) return ok;
    }
    tx.commit();
    return {};
}

// ---- Исполнения ----

namespace {
const Configuration* findConfiguration(const Document& part, const std::string& name) {
    for (const auto* c : part.objectsOf<Configuration>())
        if (c->name == name) return c;
    return nullptr;
}
}  // namespace

Result<void> ModelingService::saveConfiguration(Document& part, const std::string& name, const std::string& designation) {
    if (auto ok = requireKind(part); !ok) return ok;
    if (name.empty()) return makeError(ErrorCode::InvalidArgument, "у исполнения должно быть имя");
    std::map<std::string, std::string> values;
    for (const auto* p : part.objectsOf<Parameter>()) values[p->name] = p->expression;
    if (values.empty()) return makeError(ErrorCode::InvalidState, "у детали нет параметров: исполнению нечего запоминать");
    Transaction tx(part, "Исполнение");
    if (const Configuration* existing = findConfiguration(part, name)) {
        if (auto ok = tx.modifyAs<Configuration>(existing->id(), [&](Configuration& c) {
                c.values = values;
                c.designation = designation;
            });
            !ok)
            return ok;
    } else {
        auto configuration = std::make_unique<Configuration>();
        configuration->name = name;
        configuration->values = values;
        configuration->designation = designation;
        if (auto id = tx.add(std::move(configuration)); !id) return id.error();
    }
    tx.commit();
    return {};
}

Result<void> ModelingService::applyConfiguration(Document& part, const std::string& name) {
    if (auto ok = requireKind(part); !ok) return ok;
    const Configuration* configuration = findConfiguration(part, name);
    if (!configuration) return makeError(ErrorCode::NotFound, "исполнение «" + name + "» не найдено");
    Transaction tx(part, "Применение исполнения");
    for (const auto& [parameterName, expression] : configuration->values) {
        const Parameter* parameter = findParameter(part, parameterName);
        if (!parameter) continue;  // параметр с тех пор удалён — исполнение о нём просто молчит
        if (parameter->expression == expression) continue;
        if (auto ok = tx.modifyAs<Parameter>(parameter->id(), [&](Parameter& p) { p.expression = expression; }); !ok) return ok;
    }
    if (!configuration->designation.empty())
        for (const auto* properties : part.objectsOf<PartProperties>())
            if (auto ok = tx.modifyAs<PartProperties>(properties->id(), [&](PartProperties& p) { p.designation = configuration->designation; }); !ok)
                return ok;
    if (auto values = parameterValues(part); !values) {
        tx.rollback();
        return values.error();
    }
    tx.commit();
    return {};
}

Result<void> ModelingService::removeConfiguration(Document& part, const std::string& name) {
    if (auto ok = requireKind(part); !ok) return ok;
    const Configuration* configuration = findConfiguration(part, name);
    if (!configuration) return makeError(ErrorCode::NotFound, "исполнение «" + name + "» не найдено");
    Transaction tx(part, "Удаление исполнения");
    if (auto ok = tx.remove(configuration->id()); !ok) return ok;
    tx.commit();
    return {};
}

std::vector<std::string> ModelingService::configurations(const Document& part) const {
    std::vector<std::string> out;
    for (const auto* c : part.objectsOf<Configuration>()) out.push_back(c->name);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Перестроение ----

Result<RecomputeReport> ModelingService::recompute(const Document& part, const std::atomic<bool>* cancel) {
    if (auto ok = requireKind(part); !ok) return ok.error();
    auto values = parameterValues(part);
    if (!values) return values.error();

    DocumentCache& cache = cache_[part.id()];
    RecomputeReport report;
    std::map<Uuid, CacheEntry> fresh;
    ShapePtr body;
    std::string previousKey = "start";
    Uuid rollbackAfter;
    if (const auto properties = part.objectsOf<PartProperties>(); !properties.empty()) rollbackAfter = properties.front()->rollbackAfter;
    bool rolledBack = false;

    for (const Feature* feature : features(part)) {
        if (cancel && cancel->load()) {
            report.cancelled = true;
            return report;  // прежний результат остаётся в силе
        }
        FeatureStatus status{feature->id(), FeatureState::UpToDate, {}};
        if (rolledBack) {  // операция стоит после точки отката — ждёт
            status.state = FeatureState::Suppressed;
            status.diagnostic = "после отката";
            report.features.push_back(status);
            continue;
        }
        if (feature->id() == rollbackAfter) rolledBack = true;  // эта операция — последняя, что строится
        if (feature->suppressed) {
            status.state = FeatureState::Suppressed;
            report.features.push_back(status);
            continue;
        }

        // 1. Исходные данные операции числами. Ошибка в них — сбой операции.
        std::string inputs = std::string(toString(feature->kind)) + "|" + feature->tag + "|";
        std::optional<core::Error> problem;
        double value = 0.0;
        Plane plane;
        std::vector<Profile2D> profiles;
        if (auto v = eval(feature->value, values.value())) {
            value = v.value();
            inputs += number(value);
        } else {
            problem = v.error();
        }
        const bool usesSketch = feature->kind == FeatureKind::Extrude || feature->kind == FeatureKind::Revolve ||
                                feature->kind == FeatureKind::Sweep;
        std::vector<geometry::Point3> sweepPath;  // протяжка: точки траектории в координатах детали
        if (!problem && usesSketch) {
            const auto* sketch = part.findAs<Sketch>(feature->sketchId);
            if (!sketch) {
                problem = makeError(ErrorCode::NotFound, "эскиз операции не найден");
            } else {
                auto resolved = resolveShapes(*sketch, values.value());
                auto where = planeOf(*sketch, values.value(), kernel_, body);
                if (!resolved) {
                    problem = resolved.error();
                } else if (!where) {
                    problem = where.error();
                } else if (auto built = profilesOf(resolved.value()); !built) {
                    problem = built.error();
                } else {
                    plane = where.value();
                    profiles = built.take();
                    // Массив: те же контуры, сдвинутые или повёрнутые; у каждой копии свои метки граней.
                    if (feature->pattern.kind != Pattern::Kind::None) {
                        const Pattern& pt = feature->pattern;
                        double numbers[6] = {};
                        const std::string* texts[6] = {&pt.count, &pt.stepX, &pt.stepY, &pt.centerX, &pt.centerY, &pt.angle};
                        for (int i = 0; i < 6 && !problem; ++i) {
                            if (auto v = eval(*texts[i], values.value()))
                                numbers[i] = v.value();
                            else
                                problem = v.error();
                        }
                        const double count = numbers[0];
                        if (!problem && (count < 1.0 || count > 500.0 || std::abs(count - std::round(count)) > 1e-9))
                            problem = makeError(ErrorCode::InvalidArgument,
                                                "число копий массива должно быть целым от 1 до 500, а получилось " + number(count));
                        if (!problem) {
                            const int copies = static_cast<int>(std::lround(count));
                            const bool circular = pt.kind == Pattern::Kind::Circular;
                            // Полный круг делится на число копий; неполный угол — на число промежутков.
                            const double total = numbers[5] * 3.14159265358979323846 / 180.0;
                            const bool fullCircle = std::abs(std::abs(numbers[5]) - 360.0) < 1e-9;
                            const double stepAngle = copies > 1 ? total / (fullCircle ? copies : copies - 1) : 0.0;
                            const std::vector<Profile2D> original = profiles;
                            for (int k = 1; k < copies; ++k) {
                                const double c = std::cos(stepAngle * k), sn = std::sin(stepAngle * k);
                                const auto move = [&](Point2 p) {
                                    if (!circular) return Point2{p.x + numbers[1] * k, p.y + numbers[2] * k};
                                    const double dx = p.x - numbers[3], dy = p.y - numbers[4];
                                    return Point2{numbers[3] + dx * c - dy * sn, numbers[4] + dx * sn + dy * c};
                                };
                                const auto moveLoop = [&](std::vector<ProfileSegment> loop) {
                                    for (auto& segment : loop) {
                                        segment.start = move(segment.start);
                                        segment.end = move(segment.end);
                                        segment.center = move(segment.center);
                                        segment.sourceTag += static_cast<std::uint64_t>(k) * 1000000ULL;
                                    }
                                    return loop;
                                };
                                for (const auto& profile : original) {
                                    Profile2D copy;
                                    copy.outer = moveLoop(profile.outer);
                                    for (const auto& hole : profile.holes) copy.holes.push_back(moveLoop(hole));
                                    profiles.push_back(std::move(copy));
                                }
                            }
                            inputs += "|pattern:" + std::to_string(static_cast<int>(pt.kind));
                            for (const double v : numbers) inputs += "," + number(v);
                        }
                    }
                    if (feature->kind == FeatureKind::Sweep && !problem) {
                        const Vector3 yDirection = geometry::crossProduct(plane.normal, plane.xDirection);
                        // Траектория начинается в середине габарита контура: так контур идёт вдоль неё, а не сбоку.
                        double lowX = 1e300, lowY = 1e300, highX = -1e300, highY = -1e300;
                        const auto cover = [&](double x, double y) {
                            lowX = std::min(lowX, x);
                            lowY = std::min(lowY, y);
                            highX = std::max(highX, x);
                            highY = std::max(highY, y);
                        };
                        for (const auto& shape : resolved.value()) {
                            if (shape.kind == SketchShape::Kind::Circle) {
                                cover(shape.v[0] - shape.v[2], shape.v[1] - shape.v[2]);
                                cover(shape.v[0] + shape.v[2], shape.v[1] + shape.v[2]);
                            } else {
                                for (const auto& corner : cornersOf(shape)) cover(corner.x, corner.y);
                            }
                        }
                        const double startX = (lowX + highX) / 2.0, startY = (lowY + highY) / 2.0;
                        const geometry::Point3 start{plane.origin.x + plane.xDirection.x * startX + yDirection.x * startY,
                                                     plane.origin.y + plane.xDirection.y * startX + yDirection.y * startY,
                                                     plane.origin.z + plane.xDirection.z * startX + yDirection.z * startY};
                        sweepPath.push_back(start);
                        inputs += "|path";
                        for (const auto& point : feature->path) {
                            double c[3] = {};
                            for (int k = 0; k < 3 && !problem; ++k) {
                                if (auto v = eval(point[static_cast<std::size_t>(k)], values.value()))
                                    c[k] = v.value();
                                else
                                    problem = v.error();
                            }
                            if (problem) break;
                            sweepPath.push_back({start.x + plane.xDirection.x * c[0] + yDirection.x * c[1] + plane.normal.x * c[2],
                                                 start.y + plane.xDirection.y * c[0] + yDirection.y * c[1] + plane.normal.y * c[2],
                                                 start.z + plane.xDirection.z * c[0] + yDirection.z * c[1] + plane.normal.z * c[2]});
                            inputs += ":" + number(c[0]) + "," + number(c[1]) + "," + number(c[2]);
                        }
                    }
                    inputs += "|" + describe(plane) + "|" + describe(resolved.value()) + "|" +
                              (feature->cut ? "cut" : "add") + (feature->reversed ? "|rev" : "") + "|" + feature->axis;
                }
            }
        }
        for (const auto& edge : feature->edges) inputs += "|" + edge.faceA + "^" + edge.faceB;
        if (feature->kind == FeatureKind::Mirror) inputs += "|plane:" + feature->axis;
        if (problem) inputs += "|error:" + problem->message;
        const std::string key = fingerprint(previousKey + "|" + inputs);
        previousKey = key;

        // 2. Те же данные, что в прошлый раз, — берём готовый результат.
        if (const auto it = cache.entries.find(feature->id()); it != cache.entries.end() && it->second.key == key) {
            body = it->second.body;
            report.features.push_back(it->second.status);
            fresh[feature->id()] = it->second;
            ++report.reused;
            continue;
        }

        // 3. Расчёт.
        ++report.recomputed;
        Result<ShapePtr> result = body;
        if (problem) {
            result = *problem;
        } else if (usesSketch) {
            ShapePtr tool;
            for (const auto& profile : profiles) {
                Result<ShapePtr> piece = ShapePtr();
                if (feature->kind == FeatureKind::Sweep) {
                    piece = kernel_.sweep(profile, plane, sweepPath, value, feature->tag, cancel);
                } else if (feature->kind == FeatureKind::Extrude) {
                    piece = kernel_.extrude(profile, plane, feature->reversed ? -value : value, feature->tag, cancel);
                } else {
                    const Vector3 y = geometry::crossProduct(plane.normal, plane.xDirection);
                    piece = kernel_.revolve(profile, plane, plane.origin, feature->axis == "X" ? plane.xDirection : y,
                                            value, feature->tag, cancel);
                }
                if (!piece) {
                    result = piece.error();
                    tool.reset();
                    break;
                }
                if (!tool) {
                    tool = piece.value();
                } else if (auto joined = kernel_.boolean(geometry::BooleanKind::Union, tool, piece.value(), cancel)) {
                    tool = joined.value();
                } else {
                    result = joined.error();
                    tool.reset();
                    break;
                }
            }
            if (tool) {
                if (!body) {
                    if (feature->cut)
                        result = makeError(ErrorCode::InvalidState, "вырезать не из чего: тела ещё нет");
                    else
                        result = tool;
                } else {
                    result = kernel_.boolean(feature->cut ? geometry::BooleanKind::Subtract : geometry::BooleanKind::Union,
                                             body, tool, cancel);
                }
            }
        } else {
            if (!body) {
                result = makeError(ErrorCode::InvalidState, "тела ещё нет");
            } else if (feature->kind == FeatureKind::Mirror) {
                // Плоскость отражения проходит через начало координат и сдвинута вдоль своей нормали.
                const Vector3 normal = feature->axis == "XY" ? Vector3{0, 0, 1}
                                       : feature->axis == "XZ" ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
                const geometry::Point3 point{normal.x * value, normal.y * value, normal.z * value};
                auto copy = kernel_.mirrored(body, point, normal, feature->tag);
                if (!copy)
                    result = copy.error();
                else
                    result = kernel_.boolean(geometry::BooleanKind::Union, body, copy.value(), cancel);
            } else if (feature->kind == FeatureKind::Fillet)
                result = kernel_.fillet(body, feature->edges, value, feature->tag);
            else
                result = kernel_.chamfer(body, feature->edges, value, feature->tag);
        }

        if (result) {
            body = result.value();
        } else {
            if (result.error().code == ErrorCode::Cancelled) {
                report.cancelled = true;
                return report;
            }
            status.state = FeatureState::Failed;
            status.diagnostic = result.error().message;
        }
        report.features.push_back(status);
        fresh[feature->id()] = CacheEntry{key, body, status};
    }

    cache.entries = std::move(fresh);
    cache.body = body;
    cache.statuses = report.features;
    return report;
}

ShapePtr ModelingService::body(const Document& part) const {
    const auto it = cache_.find(part.id());
    return it == cache_.end() ? ShapePtr() : it->second.body;
}

std::vector<FeatureStatus> ModelingService::status(const Document& part) const {
    const auto it = cache_.find(part.id());
    return it == cache_.end() ? std::vector<FeatureStatus>() : it->second.statuses;
}

Result<Plane> ModelingService::facePlane(const Document& part, const std::string& faceName) const {
    Sketch probe;
    probe.plane.clear();
    probe.faceName = faceName;
    return planeOf(probe, {}, kernel_, body(part));
}

Result<PartMass> ModelingService::mass(const Document& part) const {
    const ShapePtr shape = body(part);
    if (!shape) return makeError(ErrorCode::InvalidState, "тела нет: модель пуста или не перестроена");
    auto properties = kernel_.massProperties(shape);
    if (!properties) return properties.error();
    double density = 7.85;
    if (const auto list = part.objectsOf<PartProperties>(); !list.empty()) density = list.front()->density;
    PartMass out;
    out.geometry = properties.value();
    out.massGrams = out.geometry.volume / 1000.0 * density;
    return out;
}

}  // namespace cosmocad::modeling
