// Виды чертежа по 3D-модели: линии проекции превращаются в обычные элементы чертежа.
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/IDrawingService.h"

namespace cosmocad::drawing {

using core::ErrorCode;
using core::makeError;
using document::Document;
using document::Transaction;
using geometry::ProjectedCurve;
using geometry::ProjectedView;

namespace {

BBox2 boundsOf(const ProjectedView& view) {
    BBox2 box;
    for (const auto& c : view.curves) {
        if (c.kind == ProjectedCurve::Kind::Line) {
            box.expand(c.a);
            box.expand(c.b);
        } else if (c.kind == ProjectedCurve::Kind::Circle) {
            box.expand(Point2{c.center.x - c.radius, c.center.y - c.radius});
            box.expand(Point2{c.center.x + c.radius, c.center.y + c.radius});
        } else {
            box.expand(geometry::arcBounds(c.center, c.radius, c.startAngle, c.endAngle));
        }
    }
    return box;
}

// Добавляет линии вида в документ; возвращает их номера.
Result<std::vector<Uuid>> addCurves(Transaction& tx, const ProjectedView& view, Point2 origin, double scale,
                                    const std::string& label) {
    std::vector<Uuid> ids;
    const auto place = [&](Point2 p) { return Point2{origin.x + p.x * scale, origin.y + p.y * scale}; };
    // Сначала видимые линии, потом невидимые: так видимые оказываются «сверху» при выборе.
    for (const bool hidden : {false, true}) {
        for (const auto& c : view.curves) {
            if (c.hidden != hidden) continue;
            std::unique_ptr<Entity> entity;
            if (c.kind == ProjectedCurve::Kind::Line) {
                auto line = std::make_unique<LineEntity>();
                line->start = place(c.a);
                line->end = place(c.b);
                if (geometry::distance(line->start, line->end) < 1e-6) continue;
                entity = std::move(line);
            } else if (c.kind == ProjectedCurve::Kind::Circle) {
                auto circle = std::make_unique<CircleEntity>();
                circle->center = place(c.center);
                circle->radius = c.radius * scale;
                entity = std::move(circle);
            } else {
                auto arc = std::make_unique<ArcEntity>();
                arc->center = place(c.center);
                arc->radius = c.radius * scale;
                arc->startAngle = c.startAngle;
                arc->endAngle = c.endAngle;
                entity = std::move(arc);
            }
            entity->name = hidden ? "Невидимая линия вида" : "Линия вида";
            if (hidden) entity->lineTypeId = "dashed";
            auto id = tx.add(std::move(entity));
            if (!id) return id.error();
            ids.push_back(id.value());
        }
    }
    // Разрез: штриховка там, где секущая плоскость прошла через материал (ГОСТ 2.306-68).
    // Соседние детали штрихуются в разные стороны.
    for (const auto& area : view.sectionAreas) {
        auto hatch = std::make_unique<Hatch>();
        hatch->name = "Штриховка разреза";
        hatch->lineTypeId = "solid_thin";
        hatch->angle = (area.body % 2 == 0 ? 1.0 : 3.0) * 0.7853981633974483;
        hatch->spacing = area.body % 4 < 2 ? 3.0 : 2.0;
        for (const auto& loop : area.loops) {
            std::vector<Point2> placed;
            placed.reserve(loop.size());
            for (const auto& p : loop) placed.push_back(place(p));
            hatch->loops.push_back(std::move(placed));
        }
        auto id = tx.add(std::move(hatch));
        if (!id) return id.error();
        ids.push_back(id.value());
    }
    // Надпись над изображением, по середине.
    if (!label.empty()) {
        const BBox2 box = boundsOf(view);
        auto text = std::make_unique<TextEntity>();
        text->name = "Обозначение разреза";
        text->text = label;
        text->height = 5.0;
        const double width = static_cast<double>(characterCount(label)) * text->height * 0.7;
        text->position = Point2{origin.x + (box.min.x + box.max.x) / 2.0 * scale - width / 2.0,
                                origin.y + box.max.y * scale + 4.0};
        auto id = tx.add(std::move(text));
        if (!id) return id.error();
        ids.push_back(id.value());
    }
    return ids;
}

// Обозначение секущей плоскости на другом виде: два толстых штриха за контуром, стрелки
// по направлению взгляда и буквы у стрелок.
Result<std::vector<Uuid>> addMark(Transaction& tx, const SectionMark& mark, const std::string& label) {
    std::vector<Uuid> ids;
    const auto line = [&](Point2 from, Point2 to, const char* type, const char* name) -> Result<void> {
        auto entity = std::make_unique<LineEntity>();
        entity->start = from;
        entity->end = to;
        entity->name = name;
        entity->lineTypeId = type;
        auto id = tx.add(std::move(entity));
        if (!id) return id.error();
        ids.push_back(id.value());
        return {};
    };
    const double length = geometry::distance(mark.a, mark.b);
    if (length < 1e-6) return ids;
    const Point2 dir = mark.direction;
    const std::string letter = label.substr(0, label.find("–"));
    for (const bool first : {true, false}) {
        const Point2 end = first ? mark.a : mark.b;
        const Point2 other = first ? mark.b : mark.a;
        const Point2 out{(end.x - other.x) / length, (end.y - other.y) / length};
        if (auto ok = line(end, {end.x + out.x * 10.0, end.y + out.y * 10.0}, "solid_main", "Секущая плоскость"); !ok)
            return ok.error();
        const Point2 tip{end.x + out.x * 7.0, end.y + out.y * 7.0};  // стрелка — в 3 мм от наружного конца штриха
        const Point2 tail{tip.x - dir.x * 12.0, tip.y - dir.y * 12.0};
        if (auto ok = line(tail, tip, "solid_thin", "Стрелка разреза"); !ok) return ok.error();
        for (const double side : {1.0, -1.0}) {
            const Point2 wing{tip.x - dir.x * 5.0 - dir.y * side * 0.9, tip.y - dir.y * 5.0 + dir.x * side * 0.9};
            if (auto ok = line(wing, tip, "solid_thin", "Стрелка разреза"); !ok) return ok.error();
        }
        if (!letter.empty()) {
            auto text = std::make_unique<TextEntity>();
            text->name = "Буква разреза";
            text->text = letter;
            text->height = 5.0;
            const Point2 middle{tail.x + dir.x * 2.0 + out.x * 5.0, tail.y + dir.y * 2.0 + out.y * 5.0};
            text->position = Point2{middle.x - 1.75, middle.y - 2.5};
            auto id = tx.add(std::move(text));
            if (!id) return id.error();
            ids.push_back(id.value());
        }
    }
    return ids;
}

}  // namespace

std::string viewStamp(const ProjectedView& projected) {
    // Числа округляются до 0,0001 мм, строки сортируются: порядок линий в расчёте не важен.
    const auto r = [](double v) { return std::to_string(std::llround(v * 1e4)); };
    std::vector<std::string> lines;
    for (const auto& c : projected.curves)
        lines.push_back(std::to_string(static_cast<int>(c.kind)) + (c.hidden ? "h" : "v") + r(c.a.x) + "," + r(c.a.y) + "," +
                        r(c.b.x) + "," + r(c.b.y) + "," + r(c.center.x) + "," + r(c.center.y) + "," + r(c.radius) + "," +
                        r(c.startAngle) + "," + r(c.endAngle));
    std::sort(lines.begin(), lines.end());
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& line : lines)
        for (const unsigned char ch : line + ";") hash = (hash ^ ch) * 1099511628211ULL;
    return std::to_string(lines.size()) + "-" + std::to_string(hash);
}

Result<Uuid> DrawingService::addModelView(Document& document, const ModelViewParams& params,
                                          const ProjectedView& projected) {
    if (!(params.scale > 0.0) || !std::isfinite(params.scale))
        return makeError(ErrorCode::InvalidArgument, "масштаб вида должен быть больше нуля");
    if (projected.curves.empty()) return makeError(ErrorCode::InvalidArgument, "вид пуст: у модели нет тела");
    const BBox2 box = boundsOf(projected);
    const Point2 middle = geometry::midpoint(box.min, box.max);
    const Point2 origin{params.center.x - middle.x * params.scale, params.center.y - middle.y * params.scale};

    Transaction tx(document, "Вид по модели");
    auto ids = addCurves(tx, projected, origin, params.scale, params.label);
    if (!ids) return ids.error();
    auto view = std::make_unique<ModelView>();
    view->name = "Вид по модели";
    view->sourceDocumentId = params.sourceDocumentId;
    view->view = params.view;
    view->origin = origin;
    view->scale = params.scale;
    view->showHidden = params.showHidden;
    view->section = params.section;
    view->sectionOnly = params.sectionOnly;
    view->exploded = params.exploded;
    view->stamp = viewStamp(projected);
    view->sectionAt = params.sectionAt;
    view->label = params.label;
    view->entities = ids.take();
    if (params.mark) {
        auto marks = addMark(tx, *params.mark, params.label);
        if (!marks) return marks.error();
        view->marks = marks.take();
    }
    auto id = tx.add(std::move(view));
    if (!id) return id;
    tx.commit();
    return id;
}

namespace {

const double kSame = 1e-6;

// Характерные точки вида: по ним размеры находят своё место после обновления.
struct ViewPoints {
    std::vector<double> xs, ys;                           // разные координаты, по возрастанию
    std::vector<Point2> vertices;                         // концы линий и дуг, центры окружностей
    std::vector<std::pair<Point2, double>> circles;       // центр и радиус окружностей и дуг

    void addVertex(Point2 p) {
        vertices.push_back(p);
        xs.push_back(p.x);
        ys.push_back(p.y);
    }
    void finish() {
        for (auto* list : {&xs, &ys}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end(), [](double a, double b) { return std::abs(a - b) < kSame; }),
                        list->end());
        }
    }
    // Радиусы окружностей с этим центром, по возрастанию.
    std::vector<double> radiiAt(Point2 center) const {
        std::vector<double> out;
        for (const auto& [c, r] : circles)
            if (geometry::almostEqual(c, center, kSame)) out.push_back(r);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) < kSame; }), out.end());
        return out;
    }
};

std::optional<std::size_t> rankOf(const std::vector<double>& sorted, double value) {
    for (std::size_t i = 0; i < sorted.size(); ++i)
        if (std::abs(sorted[i] - value) < kSame) return i;
    return std::nullopt;
}

void collect(const Entity& entity, ViewPoints& out) {
    if (const auto* line = dynamic_cast<const LineEntity*>(&entity)) {
        out.addVertex(line->start);
        out.addVertex(line->end);
    } else if (const auto* circle = dynamic_cast<const CircleEntity*>(&entity)) {
        out.addVertex(circle->center);
        out.circles.emplace_back(circle->center, circle->radius);
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&entity)) {
        out.addVertex(arc->center);
        out.addVertex(geometry::pointOnCircle(arc->center, arc->radius, arc->startAngle));
        out.addVertex(geometry::pointOnCircle(arc->center, arc->radius, arc->endAngle));
        out.circles.emplace_back(arc->center, arc->radius);
    }
}

}  // namespace

Result<ModelViewUpdate> DrawingService::updateModelView(Document& document, const Uuid& viewId,
                                                        const ProjectedView& projected,
                                                        const std::optional<SectionMark>& mark,
                                                        std::optional<double> newSectionAt) {
    const auto* view = document.findAs<ModelView>(viewId);
    if (!view) return makeError(ErrorCode::NotFound, "вид по модели не найден");
    if (projected.curves.empty()) return makeError(ErrorCode::InvalidArgument, "вид пуст: у модели нет тела");

    ViewPoints before;
    for (const auto& id : view->entities)
        if (const auto* entity = document.findAs<Entity>(id)) collect(*entity, before);
    before.finish();

    Transaction tx(document, "Обновление вида по модели");
    for (const auto& id : view->entities)
        if (document.find(id))  // линию могли удалить вручную
            if (auto ok = tx.remove(id); !ok) return ok.error();
    auto ids = addCurves(tx, projected, view->origin, view->scale, view->label);
    if (!ids) return ids.error();
    std::vector<Uuid> marks;
    for (const auto& id : view->marks)
        if (document.find(id))
            if (auto ok = tx.remove(id); !ok) return ok.error();
    if (mark) {
        auto drawn = addMark(tx, *mark, view->label);
        if (!drawn) return drawn.error();
        marks = drawn.take();
    }
    if (auto ok = tx.modifyAs<ModelView>(viewId, [&](ModelView& v) {
            v.entities = ids.value();
            v.marks = marks;
            v.stamp = viewStamp(projected);
            if (newSectionAt) v.sectionAt = *newSectionAt;
        });
        !ok)
        return ok.error();

    ViewPoints after;
    for (const auto& id : ids.value())
        if (const auto* entity = document.findAs<Entity>(id)) collect(*entity, after);
    after.finish();

    // Новое место точки: та же по счёту координата X и та же по счёту координата Y.
    const bool sameX = before.xs.size() == after.xs.size(), sameY = before.ys.size() == after.ys.size();
    enum class Fate { Free, Kept, Moved, Lost };
    const auto remap = [&](Point2& p) {
        // Точка считается стоящей на виде, если обе её координаты совпадают с координатами
        // характерных точек вида: это и сами точки, и углы габарита (размер общей высоты).
        const auto rx = rankOf(before.xs, p.x), ry = rankOf(before.ys, p.y);
        if (!rx || !ry) return Fate::Lost;  // размер этого вида, а точки на виде уже нет
        if (!sameX || !sameY) return Fate::Lost;
        const Point2 moved{after.xs[*rx], after.ys[*ry]};
        const bool changed = !geometry::almostEqual(moved, p, kSame);
        p = moved;
        return changed ? Fate::Moved : Fate::Kept;
    };
    // Размер принадлежит одному виду. Виды стоят рядом, и координаты их точек могут случайно
    // совпасть, поэтому принадлежность запоминается: размер, однажды поставленный на вид,
    // другими видами не трогается.
    std::set<Uuid> mine(view->dimensions.begin(), view->dimensions.end()), foreign;
    for (const auto* other : document.objectsOf<ModelView>())
        if (other->id() != viewId) foreign.insert(other->dimensions.begin(), other->dimensions.end());
    const auto onView = [&](Point2 p) { return rankOf(before.xs, p.x) && rankOf(before.ys, p.y); };
    const auto belongs = [&](const Entity& entity, bool fullyOnView) {
        if (mine.count(entity.id())) return true;
        if (foreign.count(entity.id()) || !fullyOnView) return false;
        mine.insert(entity.id());
        return true;
    };
    const std::string lostName = "Размер: привязка потеряна";
    ModelViewUpdate report;
    const auto settle = [&](const Uuid& id, bool lost, bool moved, const std::function<void(Entity&)>& apply) -> Result<void> {
        if (!lost && !moved) return {};
        if (lost && document.find(id)->name == lostName) {  // уже помечен раньше
            ++report.lostDimensions;
            return {};
        }
        auto ok = tx.modifyAs<Entity>(id, [&](Entity& e) {
            if (lost)
                e.name = lostName;
            else
                apply(e);
        });
        if (!ok) return ok;
        ++(lost ? report.lostDimensions : report.movedDimensions);
        return {};
    };
    for (const auto* entity : document.objectsOf<Entity>()) {
        if (const auto* linear = dynamic_cast<const LinearDimension*>(entity)) {
            if (!belongs(*entity, onView(linear->p1) && onView(linear->p2))) continue;
            Point2 p1 = linear->p1, p2 = linear->p2;
            const Fate f1 = remap(p1), f2 = remap(p2);
            const bool lost = f1 == Fate::Lost || f2 == Fate::Lost;
            if (auto ok = settle(entity->id(), lost, f1 == Fate::Moved || f2 == Fate::Moved, [&](Entity& e) {
                    auto& d = static_cast<LinearDimension&>(e);
                    d.p1 = p1;
                    d.p2 = p2;
                });
                !ok)
                return ok.error();
        } else if (const auto* radial = dynamic_cast<const RadialDimension*>(entity)) {
            // Размер радиуса стоит на окружности вида, если совпадают и центр, и радиус.
            const auto oldRadii = before.radiiAt(radial->center);
            const auto radiusRank = rankOf(oldRadii, radial->radius);
            if (!belongs(*entity, radiusRank.has_value() && onView(radial->center))) continue;
            if (!radiusRank) {  // окружность, на которой стоял размер, исчезла раньше
                if (auto ok = settle(entity->id(), true, false, [](Entity&) {}); !ok) return ok.error();
                continue;
            }
            Point2 center = radial->center;
            const Fate f = remap(center);
            const auto newRadii = after.radiiAt(center);
            const bool lost = f == Fate::Lost || newRadii.size() != oldRadii.size();
            const double radius = lost ? radial->radius : newRadii[*radiusRank];
            const bool moved = f == Fate::Moved || std::abs(radius - radial->radius) > kSame;
            if (auto ok = settle(entity->id(), lost, moved, [&](Entity& e) {
                    auto& d = static_cast<RadialDimension&>(e);
                    d.center = center;
                    d.radius = radius;
                });
                !ok)
                return ok.error();
        } else if (const auto* angular = dynamic_cast<const AngularDimension*>(entity)) {
            if (!belongs(*entity, onView(angular->vertex) && onView(angular->p1) && onView(angular->p2))) continue;
            Point2 v = angular->vertex, p1 = angular->p1, p2 = angular->p2;
            const Fate fates[3] = {remap(v), remap(p1), remap(p2)};
            const bool lost = std::any_of(std::begin(fates), std::end(fates), [](Fate f) { return f == Fate::Lost; });
            const bool moved = std::any_of(std::begin(fates), std::end(fates), [](Fate f) { return f == Fate::Moved; });
            if (auto ok = settle(entity->id(), lost, moved, [&](Entity& e) {
                    auto& d = static_cast<AngularDimension&>(e);
                    d.vertex = v;
                    d.p1 = p1;
                    d.p2 = p2;
                });
                !ok)
                return ok.error();
        }
    }
    // Запоминаем размеры вида; удалённые размеры из списка уходят.
    std::vector<Uuid> kept;
    for (const auto& id : mine)
        if (document.find(id)) kept.push_back(id);
    if (kept != view->dimensions)
        if (auto ok = tx.modifyAs<ModelView>(viewId, [&](ModelView& v) { v.dimensions = kept; }); !ok) return ok.error();
    tx.commit();
    return report;
}

}  // namespace cosmocad::drawing
