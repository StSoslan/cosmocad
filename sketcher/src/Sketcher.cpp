#include "cosmocad/sketcher/Sketcher.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>

#include <nlohmann/json.hpp>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/Entities.h"

namespace cosmocad::sketcher {

using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Uuid;
using document::Document;
using document::Transaction;
using drawing::ArcEntity;
using drawing::CircleEntity;
using drawing::LineEntity;
using nlohmann::json;

namespace {

struct KindName {
    ConstraintKind kind;
    const char* id;
    const char* title;
};
const KindName kKinds[] = {
    {ConstraintKind::Fixed, "fixed", "Закрепление"},
    {ConstraintKind::Coincident, "coincident", "Совпадение точек"},
    {ConstraintKind::Horizontal, "horizontal", "Горизонтальность"},
    {ConstraintKind::Vertical, "vertical", "Вертикальность"},
    {ConstraintKind::Parallel, "parallel", "Параллельность"},
    {ConstraintKind::Perpendicular, "perpendicular", "Перпендикулярность"},
    {ConstraintKind::EqualLength, "equalLength", "Равные длины"},
    {ConstraintKind::EqualRadius, "equalRadius", "Равные радиусы"},
    {ConstraintKind::Concentric, "concentric", "Концентричность"},
    {ConstraintKind::Tangent, "tangent", "Касание"},
    {ConstraintKind::PointOnLine, "pointOnLine", "Точка на прямой"},
    {ConstraintKind::Length, "length", "Длина"},
    {ConstraintKind::Radius, "radius", "Радиус"},
    {ConstraintKind::Distance, "distance", "Расстояние"},
    {ConstraintKind::Angle, "angle", "Угол"},
    {ConstraintKind::Symmetric, "symmetric", "Симметрия"},
};

const char* titleOf(ConstraintKind kind) {
    for (const auto& k : kKinds)
        if (k.kind == kind) return k.title;
    return "Ограничение";
}

}  // namespace

const char* toString(ConstraintKind kind) {
    for (const auto& k : kKinds)
        if (k.kind == kind) return k.id;
    return "fixed";
}

void Constraint::writeJson(json& j) const {
    j["kind"] = toString(kind);
    json list = json::array();
    for (const auto& ref : refs) list.push_back({{"entity", ref.entityId.toString()}, {"point", static_cast<int>(ref.point)}});
    j["refs"] = std::move(list);
    j["value"] = value;
    if (!expression.empty()) {
        j["expression"] = expression;
        j["parameters"] = parameterDocumentId.toString();
    }
}

document::Result<void> Constraint::readJson(const json& j) {
    try {
        const std::string name = j.at("kind").get<std::string>();
        bool known = false;
        for (const auto& k : kKinds)
            if (name == k.id) {
                kind = k.kind;
                known = true;
            }
        if (!known) return makeError(ErrorCode::ParseError, "ограничение: неизвестный вид «" + name + "»");
        refs.clear();
        for (const auto& item : j.at("refs")) {
            auto id = Uuid::parse(item.at("entity").get<std::string>());
            if (!id) return makeError(ErrorCode::ParseError, "ограничение: неверная ссылка на элемент");
            const int point = item.value("point", -1);
            if (point < -1 || point > 2) return makeError(ErrorCode::ParseError, "ограничение: неверная точка элемента");
            refs.push_back({id.value(), static_cast<PointRole>(point)});
        }
        value = j.value("value", 0.0);
        expression = j.value("expression", std::string());
        parameterDocumentId = Uuid();
        if (j.contains("parameters")) {
            auto id = Uuid::parse(j.at("parameters").get<std::string>());
            if (!id) return makeError(ErrorCode::ParseError, "ограничение: неверная ссылка на деталь с параметрами");
            parameterDocumentId = id.value();
        }
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("ограничение: ") + e.what());
    }
}

void registerTypes(document::ObjectFactory& factory) { factory.registerType<Constraint>(); }

// ---- Решатель ----

namespace {

enum class Shape { Line, Circle, Arc };

struct Item {
    Shape shape;
    int offset;  // где в общем списке чисел лежат числа этого элемента
    bool fixed = false;
};

// Одно условие: какие числа в нём участвуют и как считается отклонение.
struct Condition {
    Uuid constraintId;
    ConstraintKind kind;
    std::vector<int> vars;                      // номера чисел, от которых зависит условие
    std::vector<int> a, b, c;                   // номера чисел для первого, второго и третьего элемента
    double value = 0.0;
    double sign = 1.0;                          // с какой стороны касание, выбирается по начальному положению
    double scale = 1.0;                         // характерная длина: угловые отклонения переводятся в миллиметры
    int equations = 1;
};

struct System {
    std::vector<double> x;
    std::vector<bool> fixed;
    std::map<Uuid, Item> items;
    std::vector<Condition> conditions;
};

double hyp(double dx, double dy) { return std::sqrt(dx * dx + dy * dy); }

// Отклонения одного условия при числах x.
void residual(const Condition& k, const std::vector<double>& x, double* out) {
    const auto v = [&](const std::vector<int>& list, std::size_t i) { return x[static_cast<std::size_t>(list[i])]; };
    switch (k.kind) {
        case ConstraintKind::Fixed: out[0] = 0.0; break;
        case ConstraintKind::Coincident:
            out[0] = v(k.a, 0) - v(k.b, 0);
            out[1] = v(k.a, 1) - v(k.b, 1);
            break;
        case ConstraintKind::Horizontal: out[0] = v(k.a, 3) - v(k.a, 1); break;
        case ConstraintKind::Vertical: out[0] = v(k.a, 2) - v(k.a, 0); break;
        case ConstraintKind::Parallel:
        case ConstraintKind::Perpendicular:
        case ConstraintKind::Angle: {
            const double ax = v(k.a, 2) - v(k.a, 0), ay = v(k.a, 3) - v(k.a, 1);
            const double bx = v(k.b, 2) - v(k.b, 0), by = v(k.b, 3) - v(k.b, 1);
            const double la = std::max(hyp(ax, ay), 1e-12), lb = std::max(hyp(bx, by), 1e-12);
            const double cross = (ax * by - ay * bx) / (la * lb), dot = (ax * bx + ay * by) / (la * lb);
            if (k.kind == ConstraintKind::Parallel) {
                out[0] = cross * k.scale;
            } else if (k.kind == ConstraintKind::Perpendicular) {
                out[0] = dot * k.scale;
            } else {
                // Угол между направлениями отрезков, со знаком, выбранным по начальному положению.
                const double target = k.value * std::numbers::pi / 180.0 * k.sign;
                double difference = std::atan2(cross, dot) - target;
                while (difference > std::numbers::pi) difference -= 2.0 * std::numbers::pi;
                while (difference < -std::numbers::pi) difference += 2.0 * std::numbers::pi;
                out[0] = difference * k.scale;
            }
            break;
        }
        case ConstraintKind::EqualLength:
            out[0] = hyp(v(k.a, 2) - v(k.a, 0), v(k.a, 3) - v(k.a, 1)) - hyp(v(k.b, 2) - v(k.b, 0), v(k.b, 3) - v(k.b, 1));
            break;
        case ConstraintKind::Length: out[0] = hyp(v(k.a, 2) - v(k.a, 0), v(k.a, 3) - v(k.a, 1)) - k.value; break;
        case ConstraintKind::Distance: out[0] = hyp(v(k.a, 0) - v(k.b, 0), v(k.a, 1) - v(k.b, 1)) - k.value; break;
        case ConstraintKind::EqualRadius: out[0] = v(k.a, 2) - v(k.b, 2); break;
        case ConstraintKind::Radius: out[0] = v(k.a, 2) - k.value; break;
        case ConstraintKind::Concentric:
            out[0] = v(k.a, 0) - v(k.b, 0);
            out[1] = v(k.a, 1) - v(k.b, 1);
            break;
        case ConstraintKind::Tangent:
            if (k.a.size() == 4) {  // отрезок a и окружность b: расстояние от центра до прямой равно радиусу
                const double dx = v(k.a, 2) - v(k.a, 0), dy = v(k.a, 3) - v(k.a, 1);
                const double length = std::max(hyp(dx, dy), 1e-12);
                const double distance = ((v(k.b, 0) - v(k.a, 0)) * dy - (v(k.b, 1) - v(k.a, 1)) * dx) / length;
                out[0] = k.sign * distance - v(k.b, 2);
            } else {  // две окружности: снаружи (sign = 1) или одна внутри другой (sign = −1)
                const double distance = hyp(v(k.a, 0) - v(k.b, 0), v(k.a, 1) - v(k.b, 1));
                out[0] = k.sign > 0 ? distance - (v(k.a, 2) + v(k.b, 2)) : distance - std::abs(v(k.a, 2) - v(k.b, 2));
            }
            break;
        case ConstraintKind::PointOnLine: {
            const double dx = v(k.b, 2) - v(k.b, 0), dy = v(k.b, 3) - v(k.b, 1);
            out[0] = ((v(k.a, 0) - v(k.b, 0)) * dy - (v(k.a, 1) - v(k.b, 1)) * dx) / std::max(hyp(dx, dy), 1e-12);
            break;
        }
        case ConstraintKind::Symmetric: {
            // Середина между точками лежит на оси, а отрезок между ними перпендикулярен оси.
            const double dx = v(k.c, 2) - v(k.c, 0), dy = v(k.c, 3) - v(k.c, 1);
            const double length = std::max(hyp(dx, dy), 1e-12);
            const double mx = (v(k.a, 0) + v(k.b, 0)) / 2.0, my = (v(k.a, 1) + v(k.b, 1)) / 2.0;
            out[0] = ((mx - v(k.c, 0)) * dy - (my - v(k.c, 1)) * dx) / length;
            out[1] = ((v(k.b, 0) - v(k.a, 0)) * dx + (v(k.b, 1) - v(k.a, 1)) * dy) / length;
            break;
        }
    }
}

// Номера чисел точки элемента.
Result<std::vector<int>> pointVars(const System& system, const Ref& ref) {
    const auto it = system.items.find(ref.entityId);
    if (it == system.items.end()) return makeError(ErrorCode::NotFound, "элемент ограничения не найден");
    const Item& item = it->second;
    if (item.shape == Shape::Line) {
        if (ref.point == PointRole::Start) return std::vector<int>{item.offset, item.offset + 1};
        if (ref.point == PointRole::End) return std::vector<int>{item.offset + 2, item.offset + 3};
        return makeError(ErrorCode::InvalidArgument, "у отрезка точкой может быть начало или конец");
    }
    if (ref.point == PointRole::Center) return std::vector<int>{item.offset, item.offset + 1};
    return makeError(ErrorCode::InvalidArgument, "у окружности и дуги точкой может быть только центр");
}

Result<std::vector<int>> wholeVars(const System& system, const Ref& ref, Shape expected) {
    const auto it = system.items.find(ref.entityId);
    if (it == system.items.end()) return makeError(ErrorCode::NotFound, "элемент ограничения не найден");
    const Item& item = it->second;
    const bool round = item.shape != Shape::Line;
    if ((expected == Shape::Line) == round)
        return makeError(ErrorCode::InvalidArgument,
                         expected == Shape::Line ? "для этого ограничения нужен отрезок" : "для этого ограничения нужна окружность или дуга");
    std::vector<int> out;
    for (int i = 0; i < (round ? 3 : 4); ++i) out.push_back(item.offset + i);
    return out;
}

// Собирает систему: числа элементов, участвующих в ограничениях, и условия.
// extra — ограничение, которого в документе ещё нет (проверка перед добавлением).
Result<System> build(const Document& document, const Constraint* extra, const Uuid& skip = {},
                     const std::map<Uuid, double>& values = {}) {
    System system;
    std::vector<const Constraint*> all;
    for (const auto* c : document.objectsOf<Constraint>())
        if (c->id() != skip) all.push_back(c);
    if (extra) all.push_back(extra);

    const auto use = [&](const Uuid& id) -> Result<void> {
        if (system.items.count(id)) return {};
        Item item{Shape::Line, static_cast<int>(system.x.size())};
        if (const auto* line = document.findAs<LineEntity>(id)) {
            system.x.insert(system.x.end(), {line->start.x, line->start.y, line->end.x, line->end.y});
        } else if (const auto* circle = document.findAs<CircleEntity>(id)) {
            item.shape = Shape::Circle;
            system.x.insert(system.x.end(), {circle->center.x, circle->center.y, circle->radius});
        } else if (const auto* arc = document.findAs<ArcEntity>(id)) {
            item.shape = Shape::Arc;
            system.x.insert(system.x.end(), {arc->center.x, arc->center.y, arc->radius});
        } else {
            return makeError(ErrorCode::NotFound, "ограничения ставятся на отрезки, окружности и дуги; элемент не найден или другого вида");
        }
        system.items[id] = item;
        return {};
    };

    for (const Constraint* c : all) {
        for (const auto& ref : c->refs)
            if (auto ok = use(ref.entityId); !ok) return ok.error();
        const auto need = [&](std::size_t count) -> Result<void> {
            if (c->refs.size() != count)
                return makeError(ErrorCode::InvalidArgument, std::string("ограничению «") + titleOf(c->kind) + "» нужно элементов: " +
                                                                 std::to_string(count) + ", а указано " + std::to_string(c->refs.size()));
            return {};
        };
        Condition k;
        k.constraintId = c->id();
        k.kind = c->kind;
        if (const auto it = values.find(c->id()); it != values.end())
            k.value = it->second;
        else
            k.value = c->value;
        const auto whole = [&](std::size_t i, Shape shape, std::vector<int>& into) -> Result<void> {
            auto vars = wholeVars(system, c->refs[i], shape);
            if (!vars) return vars.error();
            into = vars.take();
            return {};
        };
        const auto point = [&](std::size_t i, std::vector<int>& into) -> Result<void> {
            auto vars = pointVars(system, c->refs[i]);
            if (!vars) return vars.error();
            into = vars.take();
            return {};
        };
        Result<void> ok;
        switch (c->kind) {
            case ConstraintKind::Fixed:
                if (ok = need(1); !ok) return ok.error();
                system.items[c->refs[0].entityId].fixed = true;
                k.equations = 0;
                break;
            case ConstraintKind::Coincident:
            case ConstraintKind::Distance:
                if (ok = need(2); !ok) return ok.error();
                if (ok = point(0, k.a); !ok) return ok.error();
                if (ok = point(1, k.b); !ok) return ok.error();
                k.equations = c->kind == ConstraintKind::Coincident ? 2 : 1;
                break;
            case ConstraintKind::Horizontal:
            case ConstraintKind::Vertical:
            case ConstraintKind::Length:
                if (ok = need(1); !ok) return ok.error();
                if (ok = whole(0, Shape::Line, k.a); !ok) return ok.error();
                break;
            case ConstraintKind::Parallel:
            case ConstraintKind::Perpendicular:
            case ConstraintKind::EqualLength:
            case ConstraintKind::Angle:
                if (ok = need(2); !ok) return ok.error();
                if (ok = whole(0, Shape::Line, k.a); !ok) return ok.error();
                if (ok = whole(1, Shape::Line, k.b); !ok) return ok.error();
                break;
            case ConstraintKind::EqualRadius:
            case ConstraintKind::Concentric:
                if (ok = need(2); !ok) return ok.error();
                if (ok = whole(0, Shape::Circle, k.a); !ok) return ok.error();
                if (ok = whole(1, Shape::Circle, k.b); !ok) return ok.error();
                k.equations = c->kind == ConstraintKind::Concentric ? 2 : 1;
                break;
            case ConstraintKind::Radius:
                if (ok = need(1); !ok) return ok.error();
                if (ok = whole(0, Shape::Circle, k.a); !ok) return ok.error();
                break;
            case ConstraintKind::Tangent: {
                if (ok = need(2); !ok) return ok.error();
                // Порядок элементов не важен: отрезок всегда становится первым.
                const bool firstLine = system.items[c->refs[0].entityId].shape == Shape::Line;
                const bool secondLine = system.items[c->refs[1].entityId].shape == Shape::Line;
                if (firstLine && secondLine) return makeError(ErrorCode::InvalidArgument, "касание задаётся для отрезка и окружности или для двух окружностей");
                if (firstLine || secondLine) {
                    if (ok = whole(firstLine ? 0 : 1, Shape::Line, k.a); !ok) return ok.error();
                    if (ok = whole(firstLine ? 1 : 0, Shape::Circle, k.b); !ok) return ok.error();
                } else {
                    if (ok = whole(0, Shape::Circle, k.a); !ok) return ok.error();
                    if (ok = whole(1, Shape::Circle, k.b); !ok) return ok.error();
                }
                break;
            }
            case ConstraintKind::PointOnLine:
                if (ok = need(2); !ok) return ok.error();
                if (ok = point(0, k.a); !ok) return ok.error();
                if (ok = whole(1, Shape::Line, k.b); !ok) return ok.error();
                break;
            case ConstraintKind::Symmetric:
                if (ok = need(3); !ok) return ok.error();
                if (ok = point(0, k.a); !ok) return ok.error();
                if (ok = point(1, k.b); !ok) return ok.error();
                if (ok = whole(2, Shape::Line, k.c); !ok) return ok.error();
                k.equations = 2;
                break;
        }
        if ((c->kind == ConstraintKind::Length || c->kind == ConstraintKind::Radius) && !(k.value > 0.0))
            return makeError(ErrorCode::InvalidArgument, "длина и радиус должны быть больше нуля");
        if (c->kind == ConstraintKind::Distance && !(k.value >= 0.0))
            return makeError(ErrorCode::InvalidArgument, "расстояние не может быть отрицательным");
        if (k.equations == 0) continue;
        for (const auto* list : {&k.a, &k.b, &k.c}) k.vars.insert(k.vars.end(), list->begin(), list->end());
        std::sort(k.vars.begin(), k.vars.end());
        k.vars.erase(std::unique(k.vars.begin(), k.vars.end()), k.vars.end());
        system.conditions.push_back(std::move(k));
    }

    system.fixed.assign(system.x.size(), false);
    for (const auto& [id, item] : system.items)
        if (item.fixed)
            for (int i = 0; i < (item.shape == Shape::Line ? 4 : 3); ++i) system.fixed[static_cast<std::size_t>(item.offset + i)] = true;

    // Знаки и характерные длины — по начальному положению: решатель ищет ближайшее решение.
    for (auto& k : system.conditions) {
        const auto v = [&](const std::vector<int>& list, std::size_t i) { return system.x[static_cast<std::size_t>(list[i])]; };
        if (k.kind == ConstraintKind::Parallel || k.kind == ConstraintKind::Perpendicular || k.kind == ConstraintKind::Angle) {
            const double la = hyp(v(k.a, 2) - v(k.a, 0), v(k.a, 3) - v(k.a, 1)), lb = hyp(v(k.b, 2) - v(k.b, 0), v(k.b, 3) - v(k.b, 1));
            k.scale = std::max(1.0, (la + lb) / 2.0);
            if (k.kind == ConstraintKind::Angle) {
                const double ax = v(k.a, 2) - v(k.a, 0), ay = v(k.a, 3) - v(k.a, 1);
                const double bx = v(k.b, 2) - v(k.b, 0), by = v(k.b, 3) - v(k.b, 1);
                k.sign = ax * by - ay * bx >= 0.0 ? 1.0 : -1.0;
            }
        } else if (k.kind == ConstraintKind::Tangent) {
            if (k.a.size() == 4) {
                const double dx = v(k.a, 2) - v(k.a, 0), dy = v(k.a, 3) - v(k.a, 1);
                k.sign = (v(k.b, 0) - v(k.a, 0)) * dy - (v(k.b, 1) - v(k.a, 1)) * dx >= 0.0 ? 1.0 : -1.0;
            } else {
                const double distance = hyp(v(k.a, 0) - v(k.b, 0), v(k.a, 1) - v(k.b, 1));
                k.sign = distance > std::max(v(k.a, 2), v(k.b, 2)) ? 1.0 : -1.0;
            }
        }
    }
    return system;
}

struct Outcome {
    std::vector<double> x;
    int iterations = 0;
    double worst = 0.0;
    int rank = 0;
    int equations = 0;
    int freeVariables = 0;
    double initialWorst = 0.0;
    std::vector<Uuid> unsatisfied;
};

// Ранг матрицы и решение системы методом исключения с выбором главного элемента.
int rankOf(std::vector<std::vector<double>>& m, double tolerance) {
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

bool solveLinear(std::vector<std::vector<double>> a, std::vector<double> b, std::vector<double>& x) {
    const std::size_t n = b.size();
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t best = col;
        for (std::size_t r = col + 1; r < n; ++r)
            if (std::abs(a[r][col]) > std::abs(a[best][col])) best = r;
        if (std::abs(a[best][col]) < 1e-300) return false;
        std::swap(a[best], a[col]);
        std::swap(b[best], b[col]);
        for (std::size_t r = col + 1; r < n; ++r) {
            const double k = a[r][col] / a[col][col];
            if (k == 0.0) continue;
            for (std::size_t c = col; c < n; ++c) a[r][c] -= k * a[col][c];
            b[r] -= k * b[col];
        }
    }
    x.assign(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (std::size_t c = i + 1; c < n; ++c) sum -= a[i][c] * x[c];
        x[i] = sum / a[i][i];
    }
    return true;
}

Outcome run(const System& system) {
    Outcome out;
    out.x = system.x;
    // Свободные числа получают свои номера столбцов.
    std::vector<int> column(system.x.size(), -1);
    int n = 0;
    for (std::size_t i = 0; i < system.x.size(); ++i)
        if (!system.fixed[i]) column[i] = n++;
    out.freeVariables = n;
    std::vector<int> row;  // с какой строки начинается условие
    int m = 0;
    for (const auto& k : system.conditions) {
        row.push_back(m);
        m += k.equations;
    }
    const auto residuals = [&](const std::vector<double>& x) {
        std::vector<double> r(static_cast<std::size_t>(m), 0.0);
        for (std::size_t i = 0; i < system.conditions.size(); ++i) residual(system.conditions[i], x, r.data() + row[i]);
        return r;
    };
    // Матрица производных: каждое условие зависит только от своих чисел.
    const auto jacobian = [&](std::vector<double>& x) {
        std::vector<std::vector<double>> j(static_cast<std::size_t>(m), std::vector<double>(static_cast<std::size_t>(n), 0.0));
        double plus[2], minus[2];
        for (std::size_t i = 0; i < system.conditions.size(); ++i) {
            const auto& k = system.conditions[i];
            for (const int var : k.vars) {
                const int col = column[static_cast<std::size_t>(var)];
                if (col < 0) continue;
                const double keep = x[static_cast<std::size_t>(var)], h = 1e-6;
                x[static_cast<std::size_t>(var)] = keep + h;
                residual(k, x, plus);
                x[static_cast<std::size_t>(var)] = keep - h;
                residual(k, x, minus);
                x[static_cast<std::size_t>(var)] = keep;
                for (int e = 0; e < k.equations; ++e)
                    j[static_cast<std::size_t>(row[i] + e)][static_cast<std::size_t>(col)] = (plus[e] - minus[e]) / (2.0 * h);
            }
        }
        return j;
    };
    const auto worstOf = [](const std::vector<double>& r) {
        double worst = 0.0;
        for (const double v : r) worst = std::max(worst, std::abs(v));
        return worst;
    };
    const auto costOf = [](const std::vector<double>& r) {
        double sum = 0.0;
        for (const double v : r) sum += v * v;
        return sum;
    };

    std::vector<double> r = residuals(out.x);
    out.initialWorst = worstOf(r);
    // Условия, все числа которых закреплены, ничего не ограничивают: в счёт лишних они не идут.
    out.equations = 0;
    for (const auto& k : system.conditions)
        if (std::any_of(k.vars.begin(), k.vars.end(), [&](int var) { return column[static_cast<std::size_t>(var)] >= 0; }))
            out.equations += k.equations;
    double cost = costOf(r), lambda = 1e-3;
    if (m > 0 && n > 0) {
        for (out.iterations = 0; out.iterations < 200 && worstOf(r) > 1e-10; ++out.iterations) {
            const auto j = jacobian(out.x);
            std::vector<std::vector<double>> a(static_cast<std::size_t>(n), std::vector<double>(static_cast<std::size_t>(n), 0.0));
            std::vector<double> g(static_cast<std::size_t>(n), 0.0);
            for (std::size_t e = 0; e < static_cast<std::size_t>(m); ++e) {
                // Строка почти пустая: берём только ненулевые столбцы.
                std::vector<std::pair<std::size_t, double>> filled;
                for (std::size_t c = 0; c < static_cast<std::size_t>(n); ++c)
                    if (j[e][c] != 0.0) filled.emplace_back(c, j[e][c]);
                for (const auto& [c1, v1] : filled) {
                    g[c1] += v1 * r[e];
                    for (const auto& [c2, v2] : filled) a[c1][c2] += v1 * v2;
                }
            }
            bool improved = false;
            for (int attempt = 0; attempt < 12 && !improved; ++attempt) {
                auto damped = a;
                for (std::size_t d = 0; d < static_cast<std::size_t>(n); ++d) damped[d][d] += lambda * (a[d][d] + 1e-9);
                std::vector<double> minusG(g.size()), step;
                for (std::size_t d = 0; d < g.size(); ++d) minusG[d] = -g[d];
                if (!solveLinear(std::move(damped), std::move(minusG), step)) {
                    lambda *= 10.0;
                    continue;
                }
                std::vector<double> trial = out.x;
                for (std::size_t i = 0; i < trial.size(); ++i)
                    if (column[i] >= 0) trial[i] += step[static_cast<std::size_t>(column[i])];
                const auto trialResiduals = residuals(trial);
                const double trialCost = costOf(trialResiduals);
                if (trialCost < cost) {
                    out.x = std::move(trial);
                    r = trialResiduals;
                    cost = trialCost;
                    lambda = std::max(lambda / 5.0, 1e-12);
                    improved = true;
                } else {
                    lambda *= 5.0;
                }
            }
            if (!improved) break;
        }
    }
    out.worst = worstOf(r);
    for (std::size_t i = 0; i < system.conditions.size(); ++i)
        for (int e = 0; e < system.conditions[i].equations; ++e)
            if (std::abs(r[static_cast<std::size_t>(row[i] + e)]) > 1e-6) {
                if (std::find(out.unsatisfied.begin(), out.unsatisfied.end(), system.conditions[i].constraintId) == out.unsatisfied.end())
                    out.unsatisfied.push_back(system.conditions[i].constraintId);
            }
    if (m > 0 && n > 0) {
        auto j = jacobian(out.x);
        out.rank = rankOf(j, 1e-7);
    }
    return out;
}

SolveReport reportOf(const System& system, const Outcome& outcome) {
    SolveReport report;
    report.iterations = outcome.iterations;
    report.variables = static_cast<int>(system.x.size());
    report.freedom = outcome.freeVariables - outcome.rank;
    report.redundant = outcome.equations - outcome.rank;
    report.residual = outcome.worst;
    report.unsatisfied = outcome.unsatisfied;
    report.solved = outcome.unsatisfied.empty();
    report.satisfied = outcome.initialWorst <= 1e-6;
    return report;
}

// Записывает найденные числа в элементы документа; возвращает число изменённых элементов.
Result<int> apply(Document& document, Transaction& tx, const System& system, const std::vector<double>& x) {
    int moved = 0;
    for (const auto& [id, item] : system.items) {
        const auto at = [&](int i) { return x[static_cast<std::size_t>(item.offset + i)]; };
        bool changed = false;
        for (int i = 0; i < (item.shape == Shape::Line ? 4 : 3); ++i)
            changed = changed || std::abs(at(i) - system.x[static_cast<std::size_t>(item.offset + i)]) > 1e-12;
        if (!changed) continue;
        Result<void> ok;
        if (item.shape == Shape::Line) {
            if (hyp(at(2) - at(0), at(3) - at(1)) < 1e-9) return makeError(ErrorCode::InvalidState, "ограничения стягивают отрезок в точку");
            ok = tx.modifyAs<LineEntity>(id, [&](LineEntity& line) {
                line.start = {at(0), at(1)};
                line.end = {at(2), at(3)};
            });
        } else {
            if (!(at(2) > 1e-9)) return makeError(ErrorCode::InvalidState, "ограничения дают окружность нулевого или отрицательного радиуса");
            if (item.shape == Shape::Circle)
                ok = tx.modifyAs<CircleEntity>(id, [&](CircleEntity& circle) {
                    circle.center = {at(0), at(1)};
                    circle.radius = at(2);
                });
            else
                ok = tx.modifyAs<ArcEntity>(id, [&](ArcEntity& arc) {
                    arc.center = {at(0), at(1)};
                    arc.radius = at(2);
                });
        }
        if (!ok) return ok.error();
        ++moved;
    }
    (void)document;
    return moved;
}

std::string namesOf(const Document& document, const std::vector<Uuid>& ids, const Uuid& self) {
    std::string out;
    for (const auto& id : ids) {
        if (id == self) continue;
        if (const auto* c = document.findAs<Constraint>(id)) out += (out.empty() ? "«" : ", «") + c->name + "»";
    }
    return out;
}

}  // namespace

Result<Uuid> SketchService::addConstraint(Document& document, const ConstraintParams& params) {
    auto constraint = std::make_unique<Constraint>();
    constraint->kind = params.kind;
    constraint->refs = params.refs;
    constraint->value = params.value;
    constraint->expression = params.expression;
    constraint->parameterDocumentId = params.parameterDocumentId;
    int same = 1;
    for (const auto* c : document.objectsOf<Constraint>()) same += c->kind == params.kind ? 1 : 0;
    constraint->name = std::string(titleOf(params.kind)) + " " + std::to_string(same);
    for (std::size_t i = 0; i < params.refs.size(); ++i)
        for (std::size_t j = i + 1; j < params.refs.size(); ++j)
            if (params.refs[i].entityId == params.refs[j].entityId && params.refs[i].point == params.refs[j].point)
                return makeError(ErrorCode::InvalidArgument, "ограничение связывает разные элементы или разные точки");

    auto system = build(document, constraint.get());
    if (!system) return system.error();
    const Outcome outcome = run(system.value());
    if (!outcome.unsatisfied.empty()) {
        const std::string others = namesOf(document, outcome.unsatisfied, constraint->id());
        return makeError(ErrorCode::InvalidState, std::string("ограничение «") + titleOf(params.kind) + "» не удаётся выполнить" +
                                                      (others.empty() ? std::string(" при закреплённых элементах") : " вместе с " + others));
    }
    Transaction tx(document, titleOf(params.kind));
    auto id = tx.add(std::move(constraint));
    if (!id) return id;
    if (auto moved = apply(document, tx, system.value(), outcome.x); !moved) {
        tx.rollback();
        return moved.error();
    }
    tx.commit();
    return id;
}

Result<void> SketchService::removeConstraint(Document& document, const Uuid& constraintId) {
    if (!document.findAs<Constraint>(constraintId)) return makeError(ErrorCode::NotFound, "ограничение не найдено");
    Transaction tx(document, "Удаление ограничения");
    if (auto ok = tx.remove(constraintId); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> SketchService::setValue(Document& document, const Uuid& constraintId, double value) {
    auto moved = setValues(document, {{constraintId, value}});
    if (!moved) return moved.error();
    return {};
}

Result<int> SketchService::setValues(Document& document, const std::map<Uuid, double>& values, bool keepFormulas) {
    for (const auto& [constraintId, value] : values) {
        const auto* constraint = document.findAs<Constraint>(constraintId);
        if (!constraint) return makeError(ErrorCode::NotFound, "ограничение не найдено");
        if (constraint->kind != ConstraintKind::Length && constraint->kind != ConstraintKind::Radius &&
            constraint->kind != ConstraintKind::Distance && constraint->kind != ConstraintKind::Angle)
            return makeError(ErrorCode::InvalidArgument, "у ограничения «" + constraint->name + "» нет значения");
        if (!std::isfinite(value)) return makeError(ErrorCode::InvalidArgument, "недопустимое значение ограничения");
    }
    auto system = build(document, nullptr, {}, values);
    if (!system) return system.error();
    const Outcome outcome = run(system.value());
    if (!outcome.unsatisfied.empty()) {
        const std::string others = namesOf(document, outcome.unsatisfied, values.size() == 1 ? values.begin()->first : Uuid());
        return makeError(ErrorCode::InvalidState,
                         "такое значение не удаётся выполнить" + (others.empty() ? std::string() : " вместе с " + others));
    }
    Transaction tx(document, "Значение ограничения");
    for (const auto& [constraintId, value] : values)
        if (auto ok = tx.modifyAs<Constraint>(constraintId, [&](Constraint& c) {
                c.value = value;
                if (!keepFormulas) c.expression.clear();
            });
            !ok)
            return ok.error();
    auto moved = apply(document, tx, system.value(), outcome.x);
    if (!moved) {
        tx.rollback();
        return moved.error();
    }
    tx.commit();
    return moved.value();
}

Result<int> SketchService::removeOrphans(Document& document) {
    std::vector<Uuid> orphans;
    for (const auto* c : document.objectsOf<Constraint>())
        for (const auto& ref : c->refs)
            if (!document.find(ref.entityId)) {
                orphans.push_back(c->id());
                break;
            }
    if (orphans.empty()) return 0;
    Transaction tx(document, "Удаление осиротевших ограничений");
    for (const auto& id : orphans)
        if (auto ok = tx.remove(id); !ok) return ok.error();
    tx.commit();
    return static_cast<int>(orphans.size());
}

Result<SolveReport> SketchService::solve(Document& document) {
    auto system = build(document, nullptr);
    if (!system) return system.error();
    const Outcome outcome = run(system.value());
    SolveReport report = reportOf(system.value(), outcome);
    if (!report.solved) return report;  // противоречие: геометрия не трогается
    Transaction tx(document, "Пересчёт ограничений");
    auto moved = apply(document, tx, system.value(), outcome.x);
    if (!moved) {
        tx.rollback();
        return moved.error();
    }
    report.movedEntities = moved.value();
    if (moved.value() > 0)
        tx.commit();
    else
        tx.rollback();
    return report;
}

Result<SolveReport> SketchService::analyze(const Document& document) const {
    auto system = build(document, nullptr);
    if (!system) return system.error();
    return reportOf(system.value(), run(system.value()));
}

std::vector<const Constraint*> SketchService::constraintsOf(const Document& document, const Uuid& entityId) const {
    std::vector<const Constraint*> out;
    for (const auto* c : document.objectsOf<Constraint>())
        for (const auto& ref : c->refs)
            if (ref.entityId == entityId) {
                out.push_back(c);
                break;
            }
    return out;
}

}  // namespace cosmocad::sketcher
