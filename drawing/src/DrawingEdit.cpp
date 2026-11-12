// Размеры радиуса и угла, штриховка, обрезка, скругление и фаска.
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/IDrawingService.h"

namespace cosmocad::drawing {

using core::ErrorCode;
using core::makeError;
using document::Document;
using document::Transaction;

namespace {

bool finite(Point2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

Result<void> checkLayer(const Document& document, const Uuid& layer) {
    if (layer.isNil() || document.findAs<Layer>(layer)) return {};
    return makeError(ErrorCode::NotFound, "слой не найден");
}

// Концы совпадают, если расстояние между ними меньше этого допуска, мм.
constexpr double kJoinTolerance = 1e-4;

}  // namespace

Result<Uuid> DrawingService::addRadialDimension(Document& document, Point2 center, double radius, double angle,
                                                bool diameter, const Uuid& layer) {
    if (!finite(center) || !std::isfinite(radius) || !std::isfinite(angle) || radius < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "недопустимые параметры размера");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    auto dimension = std::make_unique<RadialDimension>();
    dimension->center = center;
    dimension->radius = radius;
    dimension->angle = geometry::normalizeAngle(angle);
    dimension->diameter = diameter;
    dimension->lineTypeId = "solid_thin";
    dimension->layerId = layer;
    Transaction tx(document, diameter ? "Размер диаметра" : "Размер радиуса");
    auto id = tx.add(std::move(dimension));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<Uuid> DrawingService::addAngularDimension(Document& document, Point2 vertex, Point2 p1, Point2 p2,
                                                 double arcRadius, const Uuid& layer) {
    if (!finite(vertex) || !finite(p1) || !finite(p2) || !std::isfinite(arcRadius))
        return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    if (arcRadius < geometry::kLinearTolerance) return makeError(ErrorCode::InvalidArgument, "радиус размерной дуги должен быть больше нуля");
    if (geometry::distance(vertex, p1) < geometry::kLinearTolerance ||
        geometry::distance(vertex, p2) < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "точка на стороне угла совпадает с вершиной");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    auto dimension = std::make_unique<AngularDimension>();
    dimension->vertex = vertex;
    dimension->p1 = p1;
    dimension->p2 = p2;
    dimension->arcRadius = arcRadius;
    dimension->lineTypeId = "solid_thin";
    dimension->layerId = layer;
    if (dimension->valueDegrees() < 1e-6) return makeError(ErrorCode::InvalidArgument, "стороны угла совпадают");
    Transaction tx(document, "Угловой размер");
    auto id = tx.add(std::move(dimension));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<Uuid> DrawingService::addHatch(Document& document, const std::vector<Uuid>& boundary, double angle,
                                      double spacing, const Uuid& layer) {
    if (!std::isfinite(angle) || !std::isfinite(spacing) || spacing < 0.05)
        return makeError(ErrorCode::InvalidArgument, "шаг штриховки должен быть не меньше 0,05 мм");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    if (boundary.empty()) return makeError(ErrorCode::InvalidArgument, "не выбран контур штриховки");

    auto hatch = std::make_unique<Hatch>();
    std::vector<std::vector<Point2>> pieces;  // незамкнутые куски: отрезки и дуги
    for (const Uuid& id : boundary) {
        if (auto* line = document.findAs<LineEntity>(id)) {
            pieces.push_back({line->start, line->end});
        } else if (auto* arc = document.findAs<ArcEntity>(id)) {
            pieces.push_back(geometry::tessellateArc(arc->center, arc->radius, arc->startAngle, arc->endAngle));
        } else if (auto* circle = document.findAs<CircleEntity>(id)) {
            auto loop = geometry::tessellateArc(circle->center, circle->radius, 0.0, 0.0);
            loop.pop_back();  // последняя точка совпадает с первой
            hatch->loops.push_back(std::move(loop));
        } else if (document.find(id)) {
            return makeError(ErrorCode::InvalidArgument, "контур штриховки может состоять только из отрезков, дуг и окружностей");
        } else {
            return makeError(ErrorCode::NotFound, "элемент контура не найден");
        }
    }

    // Соединяем куски концами в замкнутые контуры.
    std::vector<bool> used(pieces.size(), false);
    for (std::size_t first = 0; first < pieces.size(); ++first) {
        if (used[first]) continue;
        used[first] = true;
        std::vector<Point2> loop = pieces[first];
        while (geometry::distance(loop.front(), loop.back()) > kJoinTolerance) {
            bool extended = false;
            for (std::size_t i = 0; i < pieces.size() && !extended; ++i) {
                if (used[i]) continue;
                std::vector<Point2> next = pieces[i];
                if (geometry::distance(next.back(), loop.back()) <= kJoinTolerance) std::reverse(next.begin(), next.end());
                if (geometry::distance(next.front(), loop.back()) > kJoinTolerance) continue;
                loop.insert(loop.end(), next.begin() + 1, next.end());
                used[i] = true;
                extended = true;
            }
            if (!extended) return makeError(ErrorCode::InvalidArgument, "контур штриховки не замкнут: концы линий не сходятся");
        }
        loop.pop_back();  // замыкающая точка совпадает с первой
        if (loop.size() < 3) return makeError(ErrorCode::InvalidArgument, "контур штриховки вырожден");
        hatch->loops.push_back(std::move(loop));
    }
    if (hatch->loops.empty()) return makeError(ErrorCode::InvalidArgument, "не выбран контур штриховки");

    hatch->angle = angle;
    hatch->spacing = spacing;
    hatch->lineTypeId = "solid_thin";  // штриховка выполняется тонкими линиями
    hatch->layerId = layer;
    if (hatch->strokes().empty()) return makeError(ErrorCode::InvalidArgument, "в контур не попадает ни одной линии штриховки");
    Transaction tx(document, "Штриховка");
    auto id = tx.add(std::move(hatch));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<void> DrawingService::trimLine(Document& document, const Uuid& lineId, Point2 pick) {
    const LineEntity* line = document.findAs<LineEntity>(lineId);
    if (!line) return makeError(ErrorCode::NotFound, "обрезать можно только отрезок");
    if (!finite(pick)) return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    const Point2 a = line->start, b = line->end;
    const Point2 d = b - a;
    const double lengthSquared = geometry::dot(d, d);
    if (lengthSquared < 1e-18) return makeError(ErrorCode::InvalidArgument, "отрезок нулевой длины");
    auto at = [&](double t) { return a + d * t; };
    auto paramOf = [&](Point2 p) { return geometry::dot(p - a, d) / lengthSquared; };

    // Точки пересечения с остальными элементами — в долях длины отрезка.
    const double eps = 1e-9;
    std::vector<double> cuts;
    for (const Entity* other : document.objectsOf<Entity>()) {
        if (other->id() == lineId) continue;
        if (auto* l = dynamic_cast<const LineEntity*>(other)) {
            if (auto x = geometry::segmentIntersection(a, b, l->start, l->end)) cuts.push_back(paramOf(*x));
        } else if (auto* c = dynamic_cast<const CircleEntity*>(other)) {
            for (double t : geometry::segmentCircleParams(a, b, c->center, c->radius)) cuts.push_back(t);
        } else if (auto* arc = dynamic_cast<const ArcEntity*>(other)) {
            for (double t : geometry::segmentCircleParams(a, b, arc->center, arc->radius)) {
                Point2 p = at(t);
                if (geometry::angleOnArc(std::atan2(p.y - arc->center.y, p.x - arc->center.x), arc->startAngle, arc->endAngle))
                    cuts.push_back(t);
            }
        }
    }
    double tp = std::clamp(paramOf(pick), 0.0, 1.0);
    double lower = 0.0, upper = 1.0;
    bool found = false;
    for (double t : cuts) {
        if (t <= eps || t >= 1.0 - eps) continue;  // пересечение в самом конце ничего не отрезает
        found = true;
        if (t <= tp) lower = std::max(lower, t);
        else upper = std::min(upper, t);
    }
    if (!found) return makeError(ErrorCode::InvalidState, "отрезок ни с чем не пересекается — обрезать нечем");

    Transaction tx(document, "Обрезать");
    if (lower <= eps && upper >= 1.0 - eps) {
        return makeError(ErrorCode::Internal, "не удалось определить отрезаемую часть");
    } else if (lower <= eps) {
        if (auto r = tx.modifyAs<LineEntity>(lineId, [&](LineEntity& l) { l.start = at(upper); }); !r) return r;
    } else if (upper >= 1.0 - eps) {
        if (auto r = tx.modifyAs<LineEntity>(lineId, [&](LineEntity& l) { l.end = at(lower); }); !r) return r;
    } else {
        // Вырезается середина: исходный отрезок укорачивается, остаток становится новым отрезком
        // с теми же слоем и видом линии.
        auto rest = line->duplicate();
        auto& restLine = static_cast<LineEntity&>(*rest);
        restLine.start = at(upper);
        restLine.end = b;
        if (auto r = tx.modifyAs<LineEntity>(lineId, [&](LineEntity& l) { l.end = at(lower); }); !r) return r;
        if (auto added = tx.add(std::move(rest)); !added) return added.error();
    }
    tx.commit();
    return {};
}

Result<void> DrawingService::cornerLines(Document& document, const Uuid& idA, Point2 pickA, const Uuid& idB,
                                         Point2 pickB, double size, bool chamfer) {
    const LineEntity* lineA = document.findAs<LineEntity>(idA);
    const LineEntity* lineB = document.findAs<LineEntity>(idB);
    if (!lineA || !lineB) return makeError(ErrorCode::NotFound, "нужны два отрезка");
    if (idA == idB) return makeError(ErrorCode::InvalidArgument, "выбран один и тот же отрезок");
    if (!std::isfinite(size) || size < 0.0) return makeError(ErrorCode::InvalidArgument, "размер не может быть отрицательным");
    if (!finite(pickA) || !finite(pickB)) return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");

    auto corner = geometry::lineIntersection(lineA->start, lineA->end, lineB->start, lineB->end);
    if (!corner) return makeError(ErrorCode::InvalidState, "отрезки параллельны");
    const Point2 P = *corner;

    // Для каждого отрезка: какой конец остаётся (со стороны указанной точки) и какой переносится.
    struct Side {
        bool keepStart = true;
        Point2 kept;
        Point2 unit;
        double length = 0.0;
    };
    auto sideOf = [&](const LineEntity& line, Point2 pick) {
        Side side;
        double startScore = geometry::dot(line.start - P, pick - P);
        double endScore = geometry::dot(line.end - P, pick - P);
        if (std::abs(startScore - endScore) < 1e-12)  // точка указана в самой вершине — оставляем дальний конец
            side.keepStart = geometry::distance(line.start, P) >= geometry::distance(line.end, P);
        else
            side.keepStart = startScore > endScore;
        side.kept = side.keepStart ? line.start : line.end;
        side.length = geometry::distance(side.kept, P);
        if (side.length > 1e-12) side.unit = (side.kept - P) * (1.0 / side.length);
        return side;
    };
    Side sa = sideOf(*lineA, pickA), sb = sideOf(*lineB, pickB);
    if (sa.length < geometry::kLinearTolerance || sb.length < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidState, "отрезок заканчивается в самой вершине угла");

    double cosine = std::clamp(geometry::dot(sa.unit, sb.unit), -1.0, 1.0);
    double phi = std::acos(cosine);  // угол между сторонами
    if (phi < 1e-9 || phi > std::numbers::pi - 1e-9) return makeError(ErrorCode::InvalidState, "отрезки лежат на одной прямой");

    // Расстояние от вершины до точек сопряжения.
    double setback = chamfer ? size : size / std::tan(phi / 2.0);
    if (setback > sa.length - geometry::kLinearTolerance || setback > sb.length - geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, chamfer ? "фаска не помещается на отрезках" : "скругление не помещается на отрезках: уменьшите радиус");
    const Point2 ta = P + sa.unit * setback;
    const Point2 tb = P + sb.unit * setback;

    Transaction tx(document, chamfer ? "Фаска" : "Скругление");
    auto moveEnd = [&](const Uuid& id, const Side& side, Point2 target) {
        return tx.modifyAs<LineEntity>(id, [&](LineEntity& l) { (side.keepStart ? l.end : l.start) = target; });
    };
    if (auto r = moveEnd(idA, sa, ta); !r) return r;
    if (auto r = moveEnd(idB, sb, tb); !r) return r;

    if (size > geometry::kLinearTolerance) {
        if (chamfer) {
            auto bridge = lineA->duplicate();  // слой и вид линии — как у первого отрезка
            auto& l = static_cast<LineEntity&>(*bridge);
            l.start = ta;
            l.end = tb;
            if (auto added = tx.add(std::move(bridge)); !added) return added.error();
        } else {
            Point2 bisector = sa.unit + sb.unit;
            bisector = bisector * (1.0 / geometry::length(bisector));
            const Point2 center = P + bisector * (size / std::sin(phi / 2.0));
            double angleA = std::atan2(ta.y - center.y, ta.x - center.x);
            double angleB = std::atan2(tb.y - center.y, tb.x - center.x);
            auto arc = std::make_unique<ArcEntity>();
            arc->center = center;
            arc->radius = size;
            // Из двух дуг между точками сопряжения нужна короткая.
            if (geometry::normalizeAngle(angleB - angleA) <= std::numbers::pi) {
                arc->startAngle = geometry::normalizeAngle(angleA);
                arc->endAngle = geometry::normalizeAngle(angleB);
            } else {
                arc->startAngle = geometry::normalizeAngle(angleB);
                arc->endAngle = geometry::normalizeAngle(angleA);
            }
            arc->layerId = lineA->layerId;
            arc->lineTypeId = lineA->lineTypeId;
            arc->lineWidth = lineA->lineWidth;
            if (auto added = tx.add(std::move(arc)); !added) return added.error();
        }
    }
    tx.commit();
    return {};
}

Result<void> DrawingService::filletLines(Document& document, const Uuid& lineA, Point2 pickA, const Uuid& lineB,
                                         Point2 pickB, double radius) {
    return cornerLines(document, lineA, pickA, lineB, pickB, radius, false);
}

Result<void> DrawingService::chamferLines(Document& document, const Uuid& lineA, Point2 pickA, const Uuid& lineB,
                                          Point2 pickB, double distance) {
    return cornerLines(document, lineA, pickA, lineB, pickB, distance, true);
}

Result<Uuid> DrawingService::addLeader(Document& document, Point2 tip, Point2 knee, const std::string& text,
                                       double textHeight, const Uuid& layer) {
    if (!finite(tip) || !finite(knee) || !(textHeight > 0.0) || !std::isfinite(textHeight))
        return makeError(ErrorCode::InvalidArgument, "недопустимые параметры выноски");
    if (geometry::distance(tip, knee) < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "выноска нулевой длины");
    if (text.empty()) return makeError(ErrorCode::InvalidArgument, "пустой текст");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    auto leader = std::make_unique<Leader>();
    leader->tip = tip;
    leader->knee = knee;
    leader->text = text;
    leader->textHeight = textHeight;
    leader->lineTypeId = "solid_thin";
    leader->layerId = layer;
    Transaction tx(document, "Выноска");
    auto id = tx.add(std::move(leader));
    if (!id) return id;
    tx.commit();
    return id;
}

// ---- Блоки ----

namespace {

const BlockDefinition* findBlock(const Document& document, const std::string& name) {
    for (const auto* block : document.objectsOf<BlockDefinition>())
        if (block->name == name) return block;
    return nullptr;
}

// Графика элемента в координатах листа — для включения в блок.
Result<std::vector<Primitive>> flatten(const Entity& entity) {
    std::vector<Primitive> out;
    Primitive p;
    if (const auto* line = dynamic_cast<const LineEntity*>(&entity)) {
        p.kind = Primitive::Kind::Line;
        p.a = line->start;
        p.b = line->end;
        out.push_back(p);
    } else if (const auto* circle = dynamic_cast<const CircleEntity*>(&entity)) {
        p.kind = Primitive::Kind::Circle;
        p.a = circle->center;
        p.radius = circle->radius;
        out.push_back(p);
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&entity)) {
        p.kind = Primitive::Kind::Arc;
        p.a = arc->center;
        p.radius = arc->radius;
        p.startAngle = arc->startAngle;
        p.endAngle = arc->endAngle;
        out.push_back(p);
    } else if (const auto* text = dynamic_cast<const TextEntity*>(&entity)) {
        p.kind = Primitive::Kind::Text;
        p.text = text->text;
        p.textHeight = text->height;
        const auto box = text->bounds();
        p.a = {(box.min.x + box.max.x) / 2.0, text->position.y};
        out.push_back(p);
    } else if (const auto* hatch = dynamic_cast<const Hatch*>(&entity)) {
        for (const auto& [from, to] : hatch->strokes()) {
            p.kind = Primitive::Kind::Line;
            p.a = from;
            p.b = to;
            out.push_back(p);
        }
    } else {
        out = entity.primitives();
        if (out.empty())
            return makeError(ErrorCode::InvalidArgument,
                             "элемент «" + entity.name + "» в блок не берётся: размеры и выноски ставятся поверх блока отдельно");
    }
    return out;
}

}  // namespace

std::vector<std::string> DrawingService::blockNames(const Document& document) const {
    std::vector<std::string> names;
    for (const auto* block : document.objectsOf<BlockDefinition>()) names.push_back(block->name);
    std::sort(names.begin(), names.end());
    return names;
}

Result<Uuid> DrawingService::createBlock(Document& document, const std::string& name, const std::vector<Uuid>& ids,
                                         Point2 basePoint, bool replace) {
    if (name.empty()) return makeError(ErrorCode::InvalidArgument, "у блока должно быть имя");
    if (ids.empty()) return makeError(ErrorCode::InvalidArgument, "для блока не выбрано ни одного элемента");
    if (!finite(basePoint)) return makeError(ErrorCode::InvalidArgument, "недопустимая базовая точка блока");
    std::vector<Primitive> content;
    Uuid layer;
    for (const auto& id : ids) {
        const auto* entity = document.findAs<Entity>(id);
        if (!entity) return makeError(ErrorCode::NotFound, "элемент для блока не найден");
        auto parts = flatten(*entity);
        if (!parts) return parts.error();
        layer = entity->layerId;
        for (auto& p : parts.value()) {
            p.a = {p.a.x - basePoint.x, p.a.y - basePoint.y};
            p.b = {p.b.x - basePoint.x, p.b.y - basePoint.y};
            content.push_back(std::move(p));
        }
    }
    Transaction tx(document, "Блок «" + name + "»");
    Uuid definitionId;
    if (const BlockDefinition* existing = findBlock(document, name)) {
        definitionId = existing->id();
        if (auto ok = tx.modifyAs<BlockDefinition>(definitionId, [&](BlockDefinition& b) { b.content = content; }); !ok) return ok.error();
        // Все вставки этого блока получают новую графику.
        for (const auto* reference : document.objectsOf<BlockReference>())
            if (reference->definitionId == definitionId)
                if (auto ok = tx.modifyAs<BlockReference>(reference->id(), [&](BlockReference& r) { r.content = content; }); !ok)
                    return ok.error();
    } else {
        auto block = std::make_unique<BlockDefinition>();
        block->name = name;
        block->content = content;
        auto id = tx.add(std::move(block));
        if (!id) return id;
        definitionId = id.value();
    }
    if (replace) {
        for (const auto& id : ids)
            if (auto ok = tx.remove(id); !ok) return ok.error();
        auto reference = std::make_unique<BlockReference>();
        reference->name = "Блок «" + name + "»";
        reference->definitionId = definitionId;
        reference->position = basePoint;
        reference->content = content;
        reference->layerId = layer;
        if (auto id = tx.add(std::move(reference)); !id) return id;
    }
    tx.commit();
    return definitionId;
}

Result<Uuid> DrawingService::insertBlock(Document& document, const std::string& name, Point2 position, double angleDegrees,
                                         double scale, const Uuid& layer) {
    const BlockDefinition* block = findBlock(document, name);
    if (!block) return makeError(ErrorCode::NotFound, "блок «" + name + "» не найден");
    if (!finite(position) || !std::isfinite(angleDegrees) || !(scale > 0.0) || !std::isfinite(scale))
        return makeError(ErrorCode::InvalidArgument, "недопустимые место, поворот или масштаб блока");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    auto reference = std::make_unique<BlockReference>();
    reference->name = "Блок «" + name + "»";
    reference->definitionId = block->id();
    reference->position = position;
    reference->angle = angleDegrees * std::numbers::pi / 180.0;
    reference->scale = scale;
    reference->content = block->content;
    reference->layerId = layer;
    Transaction tx(document, "Вставка блока");
    auto id = tx.add(std::move(reference));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<std::vector<Uuid>> DrawingService::explodeBlock(Document& document, const Uuid& referenceId) {
    const auto* reference = document.findAs<BlockReference>(referenceId);
    if (!reference) return makeError(ErrorCode::NotFound, "вставка блока не найдена");
    std::vector<Uuid> ids;
    Transaction tx(document, "Разбить блок");
    for (const auto& p : reference->primitives()) {
        std::unique_ptr<Entity> entity;
        if (p.kind == Primitive::Kind::Line) {
            auto line = std::make_unique<LineEntity>();
            line->start = p.a;
            line->end = p.b;
            entity = std::move(line);
        } else if (p.kind == Primitive::Kind::Circle || p.kind == Primitive::Kind::Dot) {
            auto circle = std::make_unique<CircleEntity>();
            circle->center = p.a;
            circle->radius = p.radius;
            entity = std::move(circle);
        } else if (p.kind == Primitive::Kind::Arc) {
            auto arc = std::make_unique<ArcEntity>();
            arc->center = p.a;
            arc->radius = p.radius;
            arc->startAngle = p.startAngle;
            arc->endAngle = p.endAngle;
            entity = std::move(arc);
        } else {
            auto text = std::make_unique<TextEntity>();
            text->text = p.text;
            text->height = p.textHeight;
            text->position = {p.a.x - 0.3 * p.textHeight * static_cast<double>(characterCount(p.text)), p.a.y};
            entity = std::move(text);
        }
        entity->layerId = reference->layerId;
        entity->lineTypeId = reference->lineTypeId;
        entity->lineWidth = reference->lineWidth;
        auto id = tx.add(std::move(entity));
        if (!id) return id.error();
        ids.push_back(id.value());
    }
    if (auto ok = tx.remove(referenceId); !ok) return ok.error();
    tx.commit();
    return ids;
}

// Значение параметра шероховатости пишется после символа: «Ra 0,4», «Rz 50», «Rmax 6,3», «Sm 0,63», «t50 70».
Result<void> checkRoughnessParameter(const std::string& text) {
    if (text.empty()) return {};
    for (const char* symbol : {"Ra ", "Rz ", "Rmax ", "Sm ", "S ", "t"}) {
        const std::string start(symbol);
        if (text.rfind(start, 0) == 0 && text.size() > start.size() &&
            std::isdigit(static_cast<unsigned char>(text[start.size()])))
            return {};
    }
    return makeError(ErrorCode::InvalidArgument,
                     "значение шероховатости пишется с символом параметра, например «Ra 3,2» или «Rz 50», а введено «" + text + "»");
}

Result<Uuid> DrawingService::addRoughness(Document& document, const RoughnessParams& params) {
    if (!finite(params.position) || !(params.textHeight > 0.0) || !std::isfinite(params.textHeight))
        return makeError(ErrorCode::InvalidArgument, "недопустимые параметры знака шероховатости");
    if (auto ok = checkRoughnessParameter(params.parameter); !ok) return ok.error();
    const bool shelf = !params.parameter.empty() || !params.method.empty();
    if (shelf && params.quarterTurns % 4 != 0)
        return makeError(ErrorCode::InvalidArgument,
                         "знак с надписью ставится прямо: на горизонтальную линию, выносную линию или полку выноски");
    if (auto ok = checkLayer(document, params.layer); !ok) return ok.error();
    auto sign = std::make_unique<RoughnessSign>();
    sign->name = "Шероховатость";
    sign->position = params.position;
    sign->kind = params.kind;
    sign->parameter = params.parameter;
    sign->method = params.method;
    sign->quarterTurns = ((params.quarterTurns % 4) + 4) % 4;
    sign->textHeight = params.textHeight;
    sign->lineTypeId = "solid_thin";
    sign->layerId = params.layer;
    Transaction tx(document, "Знак шероховатости");
    auto id = tx.add(std::move(sign));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<void> DrawingService::editEntity(Document& document, const Uuid& id, const EntityEdit& edit) {
    const Entity* entity = document.findAs<Entity>(id);
    if (!entity) return makeError(ErrorCode::NotFound, "элемент не найден");

    // Сначала проверяем всё, потом меняем: при любой ошибке документ остаётся прежним.
    auto positive = [](double v) { return std::isfinite(v) && v > geometry::kLinearTolerance; };
    const auto notApplicable = makeError(ErrorCode::InvalidArgument, "это свойство не относится к выбранному элементу");
    const bool isText = dynamic_cast<const TextEntity*>(entity) != nullptr;
    const bool isLeader = dynamic_cast<const Leader*>(entity) != nullptr;
    const bool isLinear = dynamic_cast<const LinearDimension*>(entity) != nullptr;
    const bool isRadial = dynamic_cast<const RadialDimension*>(entity) != nullptr;
    const bool isAngular = dynamic_cast<const AngularDimension*>(entity) != nullptr;
    const bool isCircle = dynamic_cast<const CircleEntity*>(entity) != nullptr;
    const bool isArc = dynamic_cast<const ArcEntity*>(entity) != nullptr;
    const bool isHatch = dynamic_cast<const Hatch*>(entity) != nullptr;

    if (edit.text) {
        if (!isText && !isLeader) return notApplicable;
        if (edit.text->empty()) return makeError(ErrorCode::InvalidArgument, "пустой текст");
    }
    if (edit.textHeight) {
        if (!isText && !isLeader && !isLinear && !isRadial && !isAngular) return notApplicable;
        if (!positive(*edit.textHeight)) return makeError(ErrorCode::InvalidArgument, "высота текста должна быть больше нуля");
    }
    if (edit.radius) {
        if (!isCircle && !isArc) return notApplicable;
        if (!positive(*edit.radius)) return makeError(ErrorCode::InvalidArgument, "радиус должен быть больше нуля");
    }
    if (edit.offset) {
        if (!isLinear) return notApplicable;
        if (!std::isfinite(*edit.offset)) return makeError(ErrorCode::InvalidArgument, "недопустимый отступ");
    }
    if (edit.diameter && !isRadial) return notApplicable;
    if (edit.hatchSpacing) {
        if (!isHatch) return notApplicable;
        if (!std::isfinite(*edit.hatchSpacing) || *edit.hatchSpacing < 0.05)
            return makeError(ErrorCode::InvalidArgument, "шаг штриховки должен быть не меньше 0,05 мм");
    }
    if (edit.hatchAngleDegrees) {
        if (!isHatch) return notApplicable;
        if (!std::isfinite(*edit.hatchAngleDegrees)) return makeError(ErrorCode::InvalidArgument, "недопустимый угол");
    }
    if (edit.note && !isLinear && !isRadial) return notApplicable;

    Transaction tx(document, "Изменить свойства");
    auto changed = tx.modify(id, [&](document::Object& object) {
        if (auto* e = dynamic_cast<TextEntity*>(&object)) {
            if (edit.text) e->text = *edit.text;
            if (edit.textHeight) e->height = *edit.textHeight;
        } else if (auto* e = dynamic_cast<Leader*>(&object)) {
            if (edit.text) e->text = *edit.text;
            if (edit.textHeight) e->textHeight = *edit.textHeight;
        } else if (auto* e = dynamic_cast<LinearDimension*>(&object)) {
            if (edit.textHeight) e->textHeight = *edit.textHeight;
            if (edit.offset) e->offset = *edit.offset;
            if (edit.note) e->note = *edit.note;
        } else if (auto* e = dynamic_cast<RadialDimension*>(&object)) {
            if (edit.textHeight) e->textHeight = *edit.textHeight;
            if (edit.diameter) e->diameter = *edit.diameter;
            if (edit.note) e->note = *edit.note;
        } else if (auto* e = dynamic_cast<AngularDimension*>(&object)) {
            if (edit.textHeight) e->textHeight = *edit.textHeight;
        } else if (auto* e = dynamic_cast<CircleEntity*>(&object)) {
            if (edit.radius) e->radius = *edit.radius;
        } else if (auto* e = dynamic_cast<ArcEntity*>(&object)) {
            if (edit.radius) e->radius = *edit.radius;
        } else if (auto* e = dynamic_cast<Hatch*>(&object)) {
            if (edit.hatchSpacing) e->spacing = *edit.hatchSpacing;
            if (edit.hatchAngleDegrees) e->angle = *edit.hatchAngleDegrees * std::numbers::pi / 180.0;
        }
    });
    if (!changed) return changed;
    tx.commit();
    return {};
}

}  // namespace cosmocad::drawing
