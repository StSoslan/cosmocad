#include "cosmocad/assembly/AssemblyService.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>

#include <nlohmann/json.hpp>

#include "cosmocad/document/Transaction.h"

namespace cosmocad::assembly {

using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Uuid;
using document::Document;
using document::Transaction;
using geometry::FaceInfo;
using geometry::Point3;
using geometry::Vector3;
using nlohmann::json;

const char* toString(MateKind kind) {
    switch (kind) {
        case MateKind::Coincident: return "coincident";
        case MateKind::Concentric: return "concentric";
        case MateKind::Parallel: return "parallel";
        case MateKind::Distance: return "distance";
        case MateKind::Angle: return "angle";
        case MateKind::Tangent: return "tangent";
    }
    return "coincident";
}

// ---- Объекты ----

void Component::writeJson(json& j) const {
    j["source"] = sourceDocumentId.toString();
    j["placement"] = placement.m;
    j["fixed"] = fixed;
    if (wholeInSection) j["wholeInSection"] = true;
    if (explode.x != 0.0 || explode.y != 0.0 || explode.z != 0.0) j["explode"] = {explode.x, explode.y, explode.z};
}

document::Result<void> Component::readJson(const json& j) {
    try {
        auto id = Uuid::parse(j.at("source").get<std::string>());
        if (!id) return makeError(ErrorCode::ParseError, "компонент: неверная ссылка на документ детали");
        sourceDocumentId = id.value();
        placement.m = j.at("placement").get<std::array<double, 12>>();
        fixed = j.value("fixed", false);
        wholeInSection = j.value("wholeInSection", false);
        explode = {0.0, 0.0, 0.0};
        if (j.contains("explode")) {
            const auto v = j.at("explode").get<std::array<double, 3>>();
            explode = {v[0], v[1], v[2]};
        }
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("компонент: ") + e.what());
    }
}

void Mate::writeJson(json& j) const {
    j["kind"] = toString(kind);
    j["first"] = {{"component", first.componentId.toString()}, {"face", first.faceName}};
    j["second"] = {{"component", second.componentId.toString()}, {"face", second.faceName}};
    j["value"] = value;
    j["flip"] = flip;
}

document::Result<void> Mate::readJson(const json& j) {
    try {
        const std::string name = j.at("kind").get<std::string>();
        if (name == "coincident")
            kind = MateKind::Coincident;
        else if (name == "concentric")
            kind = MateKind::Concentric;
        else if (name == "parallel")
            kind = MateKind::Parallel;
        else if (name == "distance")
            kind = MateKind::Distance;
        else if (name == "angle")
            kind = MateKind::Angle;
        else if (name == "tangent")
            kind = MateKind::Tangent;
        else
            return makeError(ErrorCode::ParseError, "сопряжение: неизвестный вид «" + name + "»");
        const auto readRef = [](const json& item, GeometryRef& ref) {
            auto id = Uuid::parse(item.at("component").get<std::string>());
            if (!id) return false;
            ref.componentId = id.value();
            ref.faceName = item.at("face").get<std::string>();
            return true;
        };
        if (!readRef(j.at("first"), first) || !readRef(j.at("second"), second))
            return makeError(ErrorCode::ParseError, "сопряжение: неверная ссылка на компонент");
        value = j.value("value", 0.0);
        flip = j.value("flip", false);
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("сопряжение: ") + e.what());
    }
}

void registerTypes(document::ObjectFactory& factory) {
    factory.registerType<Component>();
    factory.registerType<Mate>();
}

namespace {

Result<void> requireKind(const Document& assembly) {
    if (assembly.kind() != document::DocumentKind::Assembly)
        return makeError(ErrorCode::InvalidArgument, "операция применима только к документу «Сборка»");
    return {};
}

Vector3 sub(const Point3& a, const Point3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vector3 scaled(const Vector3& v, double k) { return {v.x * k, v.y * k, v.z * k}; }
Vector3 minus(const Vector3& a, const Vector3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double norm(const Vector3& v) { return std::sqrt(geometry::dotProduct(v, v)); }

// Наименьший поворот вокруг точки, переводящий направление from в направление to.
Placement rotationBetween(const Point3& about, const Vector3& from, const Vector3& to, double& angle) {
    const Vector3 a = geometry::normalized(from), b = geometry::normalized(to);
    const double cosine = std::clamp(geometry::dotProduct(a, b), -1.0, 1.0);
    angle = std::acos(cosine);
    if (angle < 1e-12) return Placement{};
    Vector3 axis = geometry::crossProduct(a, b);
    if (norm(axis) < 1e-9) {
        // Направления противоположны: годится любая ось, перпендикулярная им.
        const Vector3 seed = std::abs(a.x) < 0.9 ? Vector3{1, 0, 0} : Vector3{0, 1, 0};
        axis = geometry::crossProduct(a, seed);
    }
    return Placement::rotation(about, axis, angle);
}

struct WorldFace {
    FaceInfo::Kind kind;
    Point3 point;
    Vector3 direction;
    double radius = 0.0;
};

// Часть вектора, перпендикулярная направлению n; если её нет — любой перпендикуляр к n.
Vector3 across(const Vector3& v, const Vector3& n) {
    Vector3 p = minus(v, scaled(n, geometry::dotProduct(v, n)));
    if (norm(p) < 1e-9) p = geometry::crossProduct(n, std::abs(n.x) < 0.9 ? Vector3{1, 0, 0} : Vector3{0, 1, 0});
    return geometry::normalized(p);
}

}  // namespace

// ---- Компоненты ----

Result<Uuid> AssemblyService::addComponent(Document& assembly, const Uuid& sourceDocumentId, const Placement& placement,
                                           const std::string& name) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    if (sourceDocumentId == assembly.id()) return makeError(ErrorCode::InvalidArgument, "сборку нельзя вставить в саму себя");
    auto info = parts_.infoOf(sourceDocumentId);
    if (!info) return info.error();
    auto component = std::make_unique<Component>();
    component->sourceDocumentId = sourceDocumentId;
    component->placement = placement;
    component->fixed = assembly.objectsOf<Component>().empty();
    int sameSource = 1;
    for (const auto* c : assembly.objectsOf<Component>())
        if (c->sourceDocumentId == sourceDocumentId) ++sameSource;
    component->name = name.empty() ? info.value().name + " (" + std::to_string(sameSource) + ")" : name;
    Transaction tx(assembly, "Новый компонент");
    auto id = tx.add(std::move(component));
    if (!id) return id;
    // Источник должен давать тело; заодно это ловит сборку, вложенную саму в себя через подсборку.
    if (auto body = parts_.bodyOf(assembly.id()); !body) {
        tx.rollback();
        return body.error();
    }
    tx.commit();
    return id;
}

Result<void> AssemblyService::removeComponent(Document& assembly, const Uuid& componentId) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    if (!assembly.findAs<Component>(componentId)) return makeError(ErrorCode::NotFound, "компонент не найден");
    Transaction tx(assembly, "Удаление компонента");
    for (const auto* mate : assembly.objectsOf<Mate>())
        if (mate->first.componentId == componentId || mate->second.componentId == componentId)
            if (auto ok = tx.remove(mate->id()); !ok) return ok;
    if (auto ok = tx.remove(componentId); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> AssemblyService::setPlacement(Document& assembly, const Uuid& componentId, const Placement& placement) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    Transaction tx(assembly, "Перемещение компонента");
    if (auto ok = tx.modifyAs<Component>(componentId, [&](Component& c) { c.placement = placement; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> AssemblyService::setFixed(Document& assembly, const Uuid& componentId, bool fixed) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    Transaction tx(assembly, fixed ? "Закрепление компонента" : "Освобождение компонента");
    if (auto ok = tx.modifyAs<Component>(componentId, [&](Component& c) { c.fixed = fixed; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> AssemblyService::setWholeInSection(Document& assembly, const Uuid& componentId, bool whole) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    Transaction tx(assembly, whole ? "Не рассекать на разрезах" : "Рассекать на разрезах");
    if (auto ok = tx.modifyAs<Component>(componentId, [&](Component& c) { c.wholeInSection = whole; }); !ok) return ok;
    tx.commit();
    return {};
}

// ---- Сопряжения ----

Result<Uuid> AssemblyService::addMate(Document& assembly, const MateParams& params) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    if (params.first.componentId == params.second.componentId)
        return makeError(ErrorCode::InvalidArgument, "сопряжение связывает два разных компонента");
    const bool needCylinder = params.kind == MateKind::Concentric;
    if (params.kind == MateKind::Angle && !(std::isfinite(params.value) && params.value >= 0.0 && params.value <= 180.0))
        return makeError(ErrorCode::InvalidArgument, "угол между гранями задаётся от 0 до 180 градусов");
    int cylinders = 0;
    for (const GeometryRef* ref : {&params.first, &params.second}) {
        const auto* component = assembly.findAs<Component>(ref->componentId);
        if (!component) return makeError(ErrorCode::NotFound, "компонент сопряжения не найден");
        auto body = parts_.bodyOf(component->sourceDocumentId);
        if (!body) return body.error();
        auto info = kernel_.faceInfo(body.value(), ref->faceName);
        if (!info) return info.error();
        if (params.kind == MateKind::Tangent) {
            if (info.value().kind == FaceInfo::Kind::Other)
                return makeError(ErrorCode::InvalidArgument, "для касания нужны плоская и цилиндрическая грани; «" + ref->faceName + "» не подходит");
            cylinders += info.value().kind == FaceInfo::Kind::Cylinder ? 1 : 0;
            continue;
        }
        if (needCylinder && info.value().kind != FaceInfo::Kind::Cylinder)
            return makeError(ErrorCode::InvalidArgument, "для соосности нужны цилиндрические грани; «" + ref->faceName + "» не цилиндр");
        if (!needCylinder && info.value().kind != FaceInfo::Kind::Plane)
            return makeError(ErrorCode::InvalidArgument, "для этого сопряжения нужны плоские грани; «" + ref->faceName + "» не плоскость");
    }
    if (params.kind == MateKind::Tangent && cylinders != 1)
        return makeError(ErrorCode::InvalidArgument, "для касания нужна одна плоская грань и одна цилиндрическая");
    auto mate = std::make_unique<Mate>();
    mate->kind = params.kind;
    mate->first = params.first;
    mate->second = params.second;
    mate->value = params.value;
    mate->flip = params.flip;
    static const std::map<MateKind, const char*> titles = {{MateKind::Coincident, "Совпадение"},
                                                           {MateKind::Concentric, "Соосность"},
                                                           {MateKind::Parallel, "Параллельность"},
                                                           {MateKind::Distance, "Расстояние"},
                                                           {MateKind::Angle, "Угол"},
                                                           {MateKind::Tangent, "Касание"}};
    mate->name = std::string(titles.at(params.kind)) + " " + std::to_string(assembly.objectsOf<Mate>().size() + 1);
    Transaction tx(assembly, "Новое сопряжение");
    auto id = tx.add(std::move(mate));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<void> AssemblyService::removeMate(Document& assembly, const Uuid& mateId) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    if (!assembly.findAs<Mate>(mateId)) return makeError(ErrorCode::NotFound, "сопряжение не найдено");
    Transaction tx(assembly, "Удаление сопряжения");
    if (auto ok = tx.remove(mateId); !ok) return ok;
    tx.commit();
    return {};
}

Result<SolveReport> AssemblyService::solve(Document& assembly, const std::atomic<bool>* cancel) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    struct State {
        Placement placement;
        bool fixed;
        geometry::ShapePtr body;
    };
    std::map<Uuid, State> states;
    for (const auto* c : assembly.objectsOf<Component>()) {
        auto body = parts_.bodyOf(c->sourceDocumentId);
        if (!body) return body.error();
        states[c->id()] = State{c->placement, c->fixed, body.value()};
    }
    const auto mates = assembly.objectsOf<Mate>();

    const auto worldFace = [&](const GeometryRef& ref) -> Result<WorldFace> {
        const auto it = states.find(ref.componentId);
        if (it == states.end()) return makeError(ErrorCode::NotFound, "компонент сопряжения не найден");
        auto info = kernel_.faceInfo(it->second.body, ref.faceName);
        if (!info) return makeError(ErrorCode::NotFound, "грань «" + ref.faceName + "» больше не существует в детали");
        return WorldFace{info.value().kind, it->second.placement.apply(info.value().point),
                         geometry::normalized(it->second.placement.rotate(info.value().direction)), info.value().radius};
    };

    // Применяет одно сопряжение; возвращает величину поправки (0 — уже выполнено).
    const auto apply = [&](const Mate& mate, bool move) -> Result<double> {
        const bool moveFirst = states[mate.second.componentId].fixed && !states[mate.first.componentId].fixed;
        const GeometryRef& anchorRef = moveFirst ? mate.second : mate.first;
        const GeometryRef& moverRef = moveFirst ? mate.first : mate.second;
        auto anchorFace = worldFace(anchorRef);
        if (!anchorFace) return anchorFace.error();
        auto moverFace = worldFace(moverRef);
        if (!moverFace) return moverFace.error();
        const WorldFace a = anchorFace.value();
        WorldFace b = moverFace.value();
        const bool aCylinder = a.kind == FaceInfo::Kind::Cylinder, bCylinder = b.kind == FaceInfo::Kind::Cylinder;
        const bool fits = mate.kind == MateKind::Tangent
                              ? aCylinder != bCylinder && a.kind != FaceInfo::Kind::Other && b.kind != FaceInfo::Kind::Other
                              : aCylinder == (mate.kind == MateKind::Concentric) && bCylinder == aCylinder;
        if (!fits) return makeError(ErrorCode::InvalidState, "вид грани изменился и больше не подходит сопряжению");

        // 1. Поворот.
        Vector3 target = mate.flip ? a.direction : scaled(a.direction, -1.0);
        if (mate.kind == MateKind::Parallel || mate.kind == MateKind::Concentric)
            target = geometry::dotProduct(b.direction, a.direction) >= 0.0 ? a.direction : scaled(a.direction, -1.0);
        if (mate.kind == MateKind::Angle) {
            // Ближайшее к нынешнему направление, составляющее заданный угол с гранью-опорой.
            const double theta = mate.value * std::numbers::pi / 180.0;
            const Vector3 side = across(b.direction, a.direction);
            target = geometry::normalized(Vector3{a.direction.x * std::cos(theta) + side.x * std::sin(theta),
                                                  a.direction.y * std::cos(theta) + side.y * std::sin(theta),
                                                  a.direction.z * std::cos(theta) + side.z * std::sin(theta)});
        } else if (mate.kind == MateKind::Tangent) {
            // Ось цилиндра ложится параллельно плоскости: поворачивается то, что движется.
            target = across(b.direction, a.direction);
        }
        double angle = 0.0;
        const Placement turn = rotationBetween(b.point, b.direction, target, angle);

        // 2. Перенос (после поворота вокруг точки грани сама точка не сдвинулась).
        Vector3 shift{0, 0, 0};
        const Vector3 gap = sub(a.point, b.point);
        if (mate.kind == MateKind::Coincident) {
            shift = scaled(a.direction, geometry::dotProduct(gap, a.direction));
        } else if (mate.kind == MateKind::Distance) {
            shift = scaled(a.direction, geometry::dotProduct(gap, a.direction) + mate.value);
        } else if (mate.kind == MateKind::Concentric) {
            shift = minus(gap, scaled(a.direction, geometry::dotProduct(gap, a.direction)));
        } else if (mate.kind == MateKind::Tangent) {
            // Ось цилиндра — на расстоянии радиуса от плоскости, с наружной её стороны.
            if (bCylinder)  // движется цилиндр: a — плоскость, b.point — точка оси
                shift = scaled(a.direction, geometry::dotProduct(gap, a.direction) + b.radius);
            else  // движется плоскость с новой нормалью target: a.point — точка оси
                shift = scaled(target, geometry::dotProduct(gap, target) - a.radius);
        }
        const double error = angle * 100.0 + norm(shift);  // 0,01 рад считаем сравнимым с 1 мм
        if (move && error > 0.0) {
            State& mover = states[moverRef.componentId];
            if (mover.fixed) return error;  // оба закреплены — двигать нечего
            mover.placement = Placement::translation(shift.x, shift.y, shift.z) * turn * mover.placement;
        }
        return error;
    };

    SolveReport report;
    const double tolerance = 1e-7;
    for (report.passes = 1; report.passes <= 50; ++report.passes) {
        if (cancel && cancel->load()) return makeError(ErrorCode::Cancelled, "расчёт сопряжений отменён");
        double worst = 0.0;
        for (const auto* mate : mates)
            if (auto error = apply(*mate, true)) worst = std::max(worst, error.value());
        if (worst < tolerance) break;
    }
    for (const auto* mate : mates) {
        auto error = apply(*mate, false);
        if (!error)
            report.conflicts.emplace_back(mate->id(), error.error().message);
        else if (error.value() > 1e-5)
            report.conflicts.emplace_back(mate->id(),
                                          "сопряжение «" + mate->name + "» не удаётся выполнить вместе с остальными");
    }
    report.solved = report.conflicts.empty();

    Transaction tx(assembly, "Расчёт сопряжений");
    bool changed = false;
    for (const auto* c : assembly.objectsOf<Component>()) {
        const Placement& now = states[c->id()].placement;
        double difference = 0.0;
        for (std::size_t i = 0; i < 12; ++i) difference = std::max(difference, std::abs(now.m[i] - c->placement.m[i]));
        if (difference < 1e-12) continue;
        if (auto ok = tx.modifyAs<Component>(c->id(), [&](Component& item) { item.placement = now; }); !ok) return ok.error();
        changed = true;
    }
    if (changed)
        tx.commit();
    else
        tx.rollback();
    return report;
}

// ---- Степени свободы ----

namespace {

// Ранг матрицы методом исключения с выбором главного элемента.
int rankOf(std::vector<std::vector<double>> m, double tolerance) {
    int rank = 0;
    const std::size_t rows = m.size(), cols = rows ? m[0].size() : 0;
    for (std::size_t col = 0; col < cols && static_cast<std::size_t>(rank) < rows; ++col) {
        std::size_t best = static_cast<std::size_t>(rank);
        for (std::size_t r = best + 1; r < rows; ++r)
            if (std::abs(m[r][col]) > std::abs(m[best][col])) best = r;
        if (std::abs(m[best][col]) < tolerance) continue;
        std::swap(m[best], m[static_cast<std::size_t>(rank)]);
        const auto& pivot = m[static_cast<std::size_t>(rank)];
        for (std::size_t r = static_cast<std::size_t>(rank) + 1; r < rows; ++r) {
            const double k = m[r][col] / pivot[col];
            if (k == 0.0) continue;
            for (std::size_t c = col; c < cols; ++c) m[r][c] -= k * pivot[c];
        }
        ++rank;
    }
    return rank;
}

}  // namespace

Result<std::vector<Freedom>> AssemblyService::degreesOfFreedom(const Document& assembly) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    struct Item {
        const Component* component;
        geometry::ShapePtr body;
    };
    std::vector<Item> items;
    std::map<Uuid, std::size_t> indexOf;
    for (const auto* c : assembly.objectsOf<Component>()) {
        auto body = parts_.bodyOf(c->sourceDocumentId);
        if (!body) return body.error();
        indexOf[c->id()] = items.size();
        items.push_back({c, body.value()});
    }
    // Грани сопряжений в координатах своих деталей — один раз.
    struct Pair {
        const Mate* mate;
        std::size_t a, b;
        FaceInfo fa, fb;
    };
    std::vector<Pair> pairs;
    for (const auto* mate : assembly.objectsOf<Mate>()) {
        const auto ia = indexOf.find(mate->first.componentId), ib = indexOf.find(mate->second.componentId);
        if (ia == indexOf.end() || ib == indexOf.end()) continue;
        auto fa = kernel_.faceInfo(items[ia->second].body, mate->first.faceName);
        auto fb = kernel_.faceInfo(items[ib->second].body, mate->second.faceName);
        if (!fa || !fb) continue;  // пропавшая грань ничего не удерживает; о ней сообщает solve
        pairs.push_back({mate, ia->second, ib->second, fa.value(), fb.value()});
    }

    // Невязки всех сопряжений при заданных положениях компонентов.
    const auto residuals = [&](const std::vector<Placement>& where) {
        std::vector<double> out;
        const auto push = [&](const Vector3& v) {
            out.push_back(v.x);
            out.push_back(v.y);
            out.push_back(v.z);
        };
        for (const auto& pair : pairs) {
            const Point3 pa = where[pair.a].apply(pair.fa.point), pb = where[pair.b].apply(pair.fb.point);
            const Vector3 da = geometry::normalized(where[pair.a].rotate(pair.fa.direction));
            const Vector3 db = geometry::normalized(where[pair.b].rotate(pair.fb.direction));
            const Vector3 gap = sub(pb, pa);
            switch (pair.mate->kind) {
                case MateKind::Coincident:
                case MateKind::Distance:
                    push(geometry::crossProduct(da, db));
                    out.push_back(geometry::dotProduct(gap, da));
                    break;
                case MateKind::Parallel:
                    push(geometry::crossProduct(da, db));
                    break;
                case MateKind::Concentric:
                    push(geometry::crossProduct(da, db));
                    push(geometry::crossProduct(gap, da));
                    break;
                case MateKind::Angle: {
                    const double theta = pair.mate->value * std::numbers::pi / 180.0;
                    if (std::abs(std::sin(theta)) < 1e-6)
                        push(geometry::crossProduct(da, db));
                    else
                        out.push_back(geometry::dotProduct(da, db));
                    break;
                }
                case MateKind::Tangent: {
                    const bool aPlane = pair.fa.kind == FaceInfo::Kind::Plane;
                    const Vector3 normal = aPlane ? da : db;
                    out.push_back(geometry::dotProduct(da, db));
                    out.push_back(geometry::dotProduct(gap, normal));
                    break;
                }
            }
        }
        return out;
    };

    // Переменные: у каждого незакреплённого компонента три сдвига и три поворота вокруг своей точки.
    std::vector<std::size_t> movable;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!items[i].component->fixed) movable.push_back(i);
    std::vector<Placement> base;
    for (const auto& item : items) base.push_back(item.component->placement);
    const std::vector<double> zero = residuals(base);
    const double step = 1e-5;
    std::vector<std::vector<double>> columns;  // по столбцу на переменную
    for (const std::size_t i : movable) {
        const Point3 pivot = base[i].apply({0, 0, 0});
        for (int k = 0; k < 6; ++k) {
            std::vector<Placement> moved = base;
            const Vector3 axis{k % 3 == 0 ? 1.0 : 0.0, k % 3 == 1 ? 1.0 : 0.0, k % 3 == 2 ? 1.0 : 0.0};
            // Сдвиг на step мм или поворот на step·0,01 рад (на плече 100 мм это тот же step мм).
            moved[i] = (k < 3 ? Placement::translation(axis.x * step, axis.y * step, axis.z * step)
                              : Placement::rotation(pivot, axis, step * 0.01)) *
                       base[i];
            const std::vector<double> now = residuals(moved);
            std::vector<double> column(now.size());
            for (std::size_t r = 0; r < now.size(); ++r) column[r] = (now[r] - zero[r]) / step;
            columns.push_back(std::move(column));
        }
    }
    const auto rankWithout = [&](std::optional<std::size_t> skipped) {
        const std::size_t rows = zero.size();
        std::vector<std::vector<double>> matrix(rows);
        for (std::size_t v = 0; v < columns.size(); ++v) {
            if (skipped && v / 6 == *skipped) continue;
            for (std::size_t r = 0; r < rows; ++r) matrix[r].push_back(columns[v][r]);
        }
        if (rows == 0 || matrix[0].empty()) return 0;
        return rankOf(std::move(matrix), 1e-7);
    };
    const int full = rankWithout(std::nullopt);
    std::vector<Freedom> out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        Freedom freedom{items[i].component->id(), 0, items[i].component->fixed};
        if (!freedom.fixed) {
            const auto position = static_cast<std::size_t>(std::find(movable.begin(), movable.end(), i) - movable.begin());
            freedom.free = std::clamp(6 - (full - rankWithout(position)), 0, 6);
        }
        out.push_back(freedom);
    }
    return out;
}

Result<void> AssemblyService::setExplode(Document& assembly, const Uuid& componentId, const Vector3& offset) {
    if (auto ok = requireKind(assembly); !ok) return ok;
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
        return makeError(ErrorCode::InvalidArgument, "недопустимый сдвиг разнесённого вида");
    Transaction tx(assembly, "Разнесённый вид");
    if (auto ok = tx.modifyAs<Component>(componentId, [&](Component& c) { c.explode = offset; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<int> AssemblyService::autoExplode(Document& assembly, double distance) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    if (!(distance >= 0.0) || !std::isfinite(distance))
        return makeError(ErrorCode::InvalidArgument, "расстояние разноса должно быть нулём или больше");
    auto bodies = placedBodies(assembly, false);
    if (!bodies) return bodies.error();
    const auto components = assembly.objectsOf<Component>();
    // Середины габаритов; опора разноса — закреплённый компонент (или первый).
    std::vector<Point3> centers;
    for (const auto& body : bodies.value()) {
        auto box = kernel_.bounds(body.shape);
        if (!box) return box.error();
        centers.push_back({(box.value().min.x + box.value().max.x) / 2.0, (box.value().min.y + box.value().max.y) / 2.0,
                           (box.value().min.z + box.value().max.z) / 2.0});
    }
    std::size_t anchor = 0;
    for (std::size_t i = 0; i < components.size(); ++i)
        if (components[i]->fixed) {
            anchor = i;
            break;
        }
    Transaction tx(assembly, distance > 0.0 ? "Разнесённый вид" : "Разнос убран");
    int moved = 0;
    for (std::size_t i = 0; i < components.size(); ++i) {
        Vector3 offset{0.0, 0.0, 0.0};
        if (i != anchor && distance > 0.0) {
            Vector3 direction = sub(centers[i], centers[anchor]);
            // Наибольшая составляющая: детали разъезжаются вдоль осей, а не наискось.
            const double ax = std::abs(direction.x), ay = std::abs(direction.y), az = std::abs(direction.z);
            if (std::max({ax, ay, az}) < 1e-6)
                direction = {0.0, 0.0, 1.0};
            else if (az >= ax && az >= ay)
                direction = {0.0, 0.0, direction.z > 0 ? 1.0 : -1.0};
            else if (ax >= ay)
                direction = {direction.x > 0 ? 1.0 : -1.0, 0.0, 0.0};
            else
                direction = {0.0, direction.y > 0 ? 1.0 : -1.0, 0.0};
            offset = scaled(direction, distance);
        }
        const Vector3& now = components[i]->explode;
        if (std::abs(now.x - offset.x) + std::abs(now.y - offset.y) + std::abs(now.z - offset.z) < 1e-12) continue;
        if (auto ok = tx.modifyAs<Component>(components[i]->id(), [&](Component& c) { c.explode = offset; }); !ok) return ok.error();
        ++moved;
    }
    if (moved == 0) {
        tx.rollback();
        return 0;
    }
    tx.commit();
    return moved;
}

Result<std::vector<PlacedBody>> AssemblyService::placedBodies(const Document& assembly, bool exploded) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    std::vector<PlacedBody> out;
    for (const auto* c : assembly.objectsOf<Component>()) {
        auto body = parts_.bodyOf(c->sourceDocumentId);
        if (!body) return body.error();
        const Placement where = exploded ? Placement::translation(c->explode.x, c->explode.y, c->explode.z) * c->placement
                                         : c->placement;
        auto placed = kernel_.transformed(body.value(), where);
        if (!placed) return placed.error();
        out.push_back({c->id(), placed.value(), c->wholeInSection});
    }
    return out;
}

Result<std::vector<Interference>> AssemblyService::checkInterference(const Document& assembly,
                                                                     const std::atomic<bool>* cancel) {
    auto bodies = placedBodies(assembly);
    if (!bodies) return bodies.error();
    std::vector<Interference> out;
    for (std::size_t i = 0; i < bodies.value().size(); ++i)
        for (std::size_t j = i + 1; j < bodies.value().size(); ++j) {
            if (cancel && cancel->load()) return makeError(ErrorCode::Cancelled, "проверка пересечений отменена");
            auto volume = kernel_.commonVolume(bodies.value()[i].shape, bodies.value()[j].shape);
            if (!volume) return volume.error();
            if (volume.value() > 1e-6)
                out.push_back({bodies.value()[i].componentId, bodies.value()[j].componentId, volume.value()});
        }
    return out;
}

Result<double> AssemblyService::clearance(const Document& assembly, const Uuid& first, const Uuid& second) {
    auto bodies = placedBodies(assembly);
    if (!bodies) return bodies.error();
    geometry::ShapePtr a, b;
    for (const auto& body : bodies.value()) {
        if (body.componentId == first) a = body.shape;
        if (body.componentId == second) b = body.shape;
    }
    if (!a || !b || first == second) return makeError(ErrorCode::InvalidArgument, "зазор измеряется между двумя разными компонентами");
    return kernel_.distance(a, b);
}

Result<std::vector<BomLine>> AssemblyService::billOfMaterials(const Document& assembly) {
    if (auto ok = requireKind(assembly); !ok) return ok.error();
    std::vector<BomLine> lines;
    for (const auto* c : assembly.objectsOf<Component>()) {
        auto it = std::find_if(lines.begin(), lines.end(),
                               [&](const BomLine& line) { return line.documentId == c->sourceDocumentId; });
        if (it != lines.end()) {
            ++it->quantity;
            continue;
        }
        auto info = parts_.infoOf(c->sourceDocumentId);
        if (!info) return info.error();
        lines.push_back({c->sourceDocumentId, info.value().designation, info.value().name, info.value().material, 1,
                         info.value().massGrams});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const BomLine& a, const BomLine& b) {
        return a.designation != b.designation ? a.designation < b.designation : a.name < b.name;
    });
    return lines;
}

}  // namespace cosmocad::assembly
