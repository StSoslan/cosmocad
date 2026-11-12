#include "cosmocad/graphics/Scene.h"

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "cosmocad/drawing/Entities.h"

namespace cosmocad::graphics {

using drawing::AngularDimension;
using drawing::ArcEntity;
using drawing::CircleEntity;
using drawing::Entity;
using drawing::Hatch;
using drawing::Layer;
using drawing::LinearDimension;
using drawing::Leader;
using drawing::LineEntity;
using drawing::RadialDimension;
using drawing::Sheet;
using drawing::TextEntity;

namespace {

std::string formatDimension(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    std::replace(text.begin(), text.end(), '.', ',');  // десятичная запятая
    return text;
}

SceneItem line(Point2 a, Point2 b, const SceneItem& style) {
    SceneItem item = style;
    item.kind = SceneItem::Kind::Line;
    item.a = a;
    item.b = b;
    return item;
}

void addDimensionText(Scene& scene, Point2 anchor, Point2 direction, const std::string& value, double height,
                      SceneItem style, const drawing::DimensionNote* note = nullptr);

void addRectangle(Scene& scene, const BBox2& box, const SceneItem& style) {
    Point2 p1 = box.min, p2{box.max.x, box.min.y}, p3 = box.max, p4{box.min.x, box.max.y};
    scene.items.push_back(line(p1, p2, style));
    scene.items.push_back(line(p2, p3, style));
    scene.items.push_back(line(p3, p4, style));
    scene.items.push_back(line(p4, p1, style));
}

void addDimension(Scene& scene, const LinearDimension& dimension, SceneItem style) {
    Point2 direction = dimension.p2 - dimension.p1;
    double len = geometry::length(direction);
    if (len < geometry::kLinearTolerance) return;
    Point2 unit = direction * (1.0 / len);
    Point2 normal{-unit.y, unit.x};
    double side = dimension.offset >= 0.0 ? 1.0 : -1.0;
    Point2 d1 = dimension.p1 + normal * dimension.offset;
    Point2 d2 = dimension.p2 + normal * dimension.offset;
    double overshoot = 2.0;  // выход выносной линии за размерную, мм

    style.pattern.clear();
    scene.items.push_back(line(dimension.p1, d1 + normal * (side * overshoot), style));
    scene.items.push_back(line(dimension.p2, d2 + normal * (side * overshoot), style));
    scene.items.push_back(line(d1, d2, style));

    // Стрелки: два штриха под углом около 20° к размерной линии.
    double arrowLength = std::min(5.0, len / 3.0);
    double arrowHalfWidth = arrowLength * 0.18;
    for (int end = 0; end < 2; ++end) {
        Point2 tip = end == 0 ? d1 : d2;
        Point2 along = unit * (end == 0 ? arrowLength : -arrowLength);
        scene.items.push_back(line(tip, tip + along + normal * arrowHalfWidth, style));
        scene.items.push_back(line(tip, tip + along - normal * arrowHalfWidth, style));
    }

    addDimensionText(scene, geometry::midpoint(d1, d2), unit, formatDimension(dimension.value()), dimension.textHeight,
                     style, &dimension.note);
}

// Стрелка размерной линии: остриё в tip, тело стрелки уходит в направлении back (единичный вектор).
void addArrow(Scene& scene, Point2 tip, Point2 back, double length, const SceneItem& style) {
    Point2 normal{-back.y, back.x};
    double halfWidth = length * 0.18;
    scene.items.push_back(line(tip, tip + back * length + normal * halfWidth, style));
    scene.items.push_back(line(tip, tip + back * length - normal * halfWidth, style));
}

// Размерное число над линией: середина нижнего края строки в точке anchor,
// строка идёт вдоль direction и не переворачивается «вверх ногами».
void addDimensionText(Scene& scene, Point2 anchor, Point2 direction, const std::string& value, double height,
                      SceneItem style, const drawing::DimensionNote* note) {
    style.kind = SceneItem::Kind::Text;
    style.pattern.clear();
    style.text = value + (note ? note->suffix : std::string());
    style.textHeight = height;
    style.textCentered = true;
    double angle = std::atan2(direction.y, direction.x);
    Point2 normal{-direction.y, direction.x};
    if (angle > std::numbers::pi / 2.0 + 1e-9 || angle <= -std::numbers::pi / 2.0 + 1e-9) {
        angle += std::numbers::pi;
        normal = normal * -1.0;
    }
    style.textAngle = angle;
    style.a = anchor + normal * 1.0;
    scene.items.push_back(style);

    // Предельные отклонения: мелким шрифтом справа от числа, одно над другим.
    if (note && (!note->upper.empty() || !note->lower.empty())) {
        Point2 along{std::cos(angle), std::sin(angle)};
        double halfWidth = 0.3 * height * static_cast<double>(drawing::characterCount(style.text));
        double small = 0.7 * height;
        SceneItem deviation = style;
        deviation.textCentered = false;
        deviation.textHeight = small;
        Point2 base = anchor + along * (halfWidth + 0.7);
        if (!note->upper.empty()) {
            deviation.text = note->upper;
            deviation.a = base + normal * (1.0 + 0.6 * height);
            scene.items.push_back(deviation);
        }
        if (!note->lower.empty()) {
            deviation.text = note->lower;
            deviation.a = base + normal * (1.0 - 0.2 * height);
            scene.items.push_back(deviation);
        }
    }
}

void addLeader(Scene& scene, const Leader& leader, SceneItem style) {
    style.pattern.clear();
    double length = geometry::distance(leader.tip, leader.knee);
    if (length < geometry::kLinearTolerance) return;
    Point2 toKnee = (leader.knee - leader.tip) * (1.0 / length);
    Point2 end = leader.shelfEnd();
    scene.items.push_back(line(leader.tip, leader.knee, style));
    scene.items.push_back(line(leader.knee, end, style));
    addArrow(scene, leader.tip, toKnee, std::min(5.0, length / 3.0), style);

    SceneItem text = style;
    text.kind = SceneItem::Kind::Text;
    text.text = leader.text;
    text.textHeight = leader.textHeight;
    text.a = {std::min(leader.knee.x, end.x) + 1.0, leader.knee.y + 1.0};  // над полкой, от её левого края
    scene.items.push_back(std::move(text));
}

void addRadialDimension(Scene& scene, const RadialDimension& dimension, SceneItem style) {
    style.pattern.clear();
    Point2 unit{std::cos(dimension.angle), std::sin(dimension.angle)};
    Point2 tip = dimension.center + unit * dimension.radius;
    Point2 from = dimension.diameter ? dimension.center - unit * dimension.radius : dimension.center;
    scene.items.push_back(line(from, tip, style));
    double arrow = std::min(5.0, dimension.radius / 3.0);
    addArrow(scene, tip, unit * -1.0, arrow, style);
    if (dimension.diameter) addArrow(scene, from, unit, arrow, style);
    // Знак диаметра — «Ø» (есть во всех шрифтах), радиуса — «R».
    std::string text = (dimension.diameter ? "\u00D8" : "R") + formatDimension(dimension.value());
    Point2 anchor = dimension.diameter ? dimension.center : geometry::midpoint(dimension.center, tip);
    if (double extension = dimension.outsideExtension(); extension > 0.0) {
        Point2 outer = tip + unit * extension;
        scene.items.push_back(line(tip, outer, style));
        anchor = geometry::midpoint(tip, outer);
    }
    addDimensionText(scene, anchor, unit, text, dimension.textHeight, style, &dimension.note);
}

void addAngularDimension(Scene& scene, const AngularDimension& dimension, SceneItem style) {
    style.pattern.clear();
    const Point2 v = dimension.vertex;
    double a1 = std::atan2(dimension.p1.y - v.y, dimension.p1.x - v.x);
    double a2 = std::atan2(dimension.p2.y - v.y, dimension.p2.x - v.x);
    double sweep = geometry::normalizeAngle(a2 - a1);
    const double r = dimension.arcRadius;

    SceneItem arc = style;
    arc.kind = SceneItem::Kind::Arc;
    arc.a = v;
    arc.radius = r;
    arc.startAngle = a1;
    arc.endAngle = a2;
    scene.items.push_back(arc);

    // Выносные линии — если размерная дуга дальше от вершины, чем указанные точки сторон.
    for (auto [point, angle] : {std::pair{dimension.p1, a1}, std::pair{dimension.p2, a2}}) {
        double reach = geometry::distance(point, v);
        if (reach < r) scene.items.push_back(line(point, geometry::pointOnCircle(v, r + 2.0, angle), style));
    }
    // Стрелки направлены по касательной к дуге, остриями к её концам.
    double arrow = std::min(5.0, r * sweep / 3.0);
    Point2 start = geometry::pointOnCircle(v, r, a1), end = geometry::pointOnCircle(v, r, a2);
    addArrow(scene, start, Point2{-std::sin(a1), std::cos(a1)}, arrow, style);
    addArrow(scene, end, Point2{std::sin(a2), -std::cos(a2)}, arrow, style);

    double mid = a1 + sweep / 2.0;
    Point2 tangent{-std::sin(mid), std::cos(mid)};
    addDimensionText(scene, geometry::pointOnCircle(v, r, mid), tangent, formatDimension(dimension.valueDegrees()) + "\u00B0",
                     dimension.textHeight, style);
}

void addHatch(Scene& scene, const Hatch& hatch, SceneItem style) {
    style.pattern.clear();
    for (const auto& [a, b] : hatch.strokes()) scene.items.push_back(line(a, b, style));
}

void addSheetDecoration(Scene& scene, const Sheet& sheet, const standards::Profile& profile) {
    auto g = standards::sheetGeometry(sheet, profile);
    if (!g) return;
    scene.sheetWidth = g->width;
    scene.sheetHeight = g->height;

    SceneItem border;
    border.width = sheet.mainLineWidth / 2.0;
    border.layer = "SHEET";
    border.color = "#808080";
    addRectangle(scene, BBox2::fromCorners({0.0, 0.0}, {g->width, g->height}), border);

    SceneItem frame;
    frame.width = sheet.mainLineWidth;
    frame.layer = "FRAME";
    addRectangle(scene, g->frame, frame);

    // Основная надпись рисуется целиком по описанию из профиля: сетка, подписи, поля.
    if (profile.titleBlock.width <= 0.0 || profile.titleBlock.height <= 0.0) return;
    const BBox2& tb = g->titleBlock;
    const Point2 origin = tb.min;
    addRectangle(scene, tb, frame);

    SceneItem thin = frame;
    thin.width = sheet.mainLineWidth / 2.0;
    for (const auto& l : profile.titleBlock.lines)
        scene.items.push_back(line({origin.x + l.x1, origin.y + l.y1}, {origin.x + l.x2, origin.y + l.y2},
                                   l.thin ? thin : frame));

    auto text = [&](double x, double y, const std::string& value, double height) {
        SceneItem item;
        item.kind = SceneItem::Kind::Text;
        item.a = {origin.x + x, origin.y + y};
        item.text = value;
        item.textHeight = height;
        item.width = sheet.mainLineWidth / 2.0;
        item.layer = "FRAME";
        scene.items.push_back(std::move(item));
    };
    for (const auto& label : profile.titleBlock.labels) text(label.x, label.y, label.text, label.height);
    for (const auto& field : profile.titleBlock.fields) {
        auto it = sheet.titleBlock.find(field.key);
        if (it != sheet.titleBlock.end() && !it->second.empty()) text(field.x, field.y, it->second, field.height);
    }
}

}  // namespace

Scene buildScene(const document::Document& document, const standards::Profile* profile) {
    Scene scene;
    static const standards::Profile kEmptyProfile{};
    const standards::Profile& effective = profile ? *profile : kEmptyProfile;

    auto sheets = document.objectsOf<Sheet>();
    if (profile && !sheets.empty()) addSheetDecoration(scene, *sheets.front(), *profile);

    const double mainLineWidth = standards::mainLineWidthOf(document);
    for (const Entity* entity : document.objectsOf<Entity>()) {
        auto resolved = standards::resolveLineStyle(document, *entity, effective, mainLineWidth);
        if (!resolved.visible) continue;

        SceneItem style;
        style.width = resolved.width;
        style.pattern = resolved.pattern;
        style.color = resolved.color;
        style.source = entity->id();
        if (const Layer* layer = entity->layerId.isNil() ? nullptr : document.findAs<Layer>(entity->layerId))
            style.layer = layer->name.empty() ? "0" : layer->name;

        if (auto* l = dynamic_cast<const LineEntity*>(entity)) {
            scene.items.push_back(line(l->start, l->end, style));
        } else if (auto* c = dynamic_cast<const CircleEntity*>(entity)) {
            style.kind = SceneItem::Kind::Circle;
            style.a = c->center;
            style.radius = c->radius;
            scene.items.push_back(style);
        } else if (auto* a = dynamic_cast<const ArcEntity*>(entity)) {
            style.kind = SceneItem::Kind::Arc;
            style.a = a->center;
            style.radius = a->radius;
            style.startAngle = a->startAngle;
            style.endAngle = a->endAngle;
            scene.items.push_back(style);
        } else if (auto* t = dynamic_cast<const TextEntity*>(entity)) {
            style.kind = SceneItem::Kind::Text;
            style.a = t->position;
            style.text = t->text;
            style.textHeight = t->height;
            style.pattern.clear();
            scene.items.push_back(style);
        } else if (auto* d = dynamic_cast<const LinearDimension*>(entity)) {
            addDimension(scene, *d, style);
        } else if (auto* rd = dynamic_cast<const RadialDimension*>(entity)) {
            addRadialDimension(scene, *rd, style);
        } else if (auto* ad = dynamic_cast<const AngularDimension*>(entity)) {
            addAngularDimension(scene, *ad, style);
        } else if (auto* h = dynamic_cast<const Hatch*>(entity)) {
            addHatch(scene, *h, style);
        } else if (auto* ld = dynamic_cast<const Leader*>(entity)) {
            addLeader(scene, *ld, style);
        } else {
            // Составной объект (элемент схемы, проводник): рисуется по своему списку простых элементов.
            for (const auto& p : entity->primitives()) {
                SceneItem item = style;
                switch (p.kind) {
                    case drawing::Primitive::Kind::Line:
                        item = line(p.a, p.b, style);
                        break;
                    case drawing::Primitive::Kind::Circle:
                        item.kind = SceneItem::Kind::Circle;
                        item.a = p.a;
                        item.radius = p.radius;
                        break;
                    case drawing::Primitive::Kind::Arc:
                        item.kind = SceneItem::Kind::Arc;
                        item.a = p.a;
                        item.radius = p.radius;
                        item.startAngle = p.startAngle;
                        item.endAngle = p.endAngle;
                        break;
                    case drawing::Primitive::Kind::Text:
                        item.kind = SceneItem::Kind::Text;
                        item.a = p.a;
                        item.text = p.text;
                        item.textHeight = p.textHeight;
                        item.textCentered = true;
                        item.pattern.clear();
                        break;
                    case drawing::Primitive::Kind::Dot:
                        // Точка соединения: маленькая окружность, целиком закрытая толщиной своей линии.
                        item.kind = SceneItem::Kind::Circle;
                        item.a = p.a;
                        item.radius = p.radius / 2.0;
                        item.width = p.radius;
                        item.pattern.clear();
                        break;
                }
                scene.items.push_back(item);
            }
        }
        scene.bounds.expand(entity->bounds());
    }

    if (scene.sheetWidth > 0.0) scene.bounds.expand(BBox2::fromCorners({0.0, 0.0}, {scene.sheetWidth, scene.sheetHeight}));
    return scene;
}

// ---- ViewTransform2D ----

Point2 ViewTransform2D::toScreen(Point2 world) const {
    return {offset_.x + world.x * scale_, offset_.y - world.y * scale_};
}

Point2 ViewTransform2D::toWorld(Point2 screen) const {
    return {(screen.x - offset_.x) / scale_, (offset_.y - screen.y) / scale_};
}

void ViewTransform2D::pan(double dxScreen, double dyScreen) {
    offset_.x += dxScreen;
    offset_.y += dyScreen;
}

void ViewTransform2D::zoomAt(Point2 screenPoint, double factor) {
    if (factor <= 0.0) return;
    Point2 anchor = toWorld(screenPoint);
    scale_ = std::clamp(scale_ * factor, 1e-4, 1e5);
    offset_.x = screenPoint.x - anchor.x * scale_;
    offset_.y = screenPoint.y + anchor.y * scale_;
}

void ViewTransform2D::fit(const BBox2& box, double viewWidth, double viewHeight, double marginScreen) {
    if (!box.valid || viewWidth <= 2 * marginScreen || viewHeight <= 2 * marginScreen) return;
    double w = std::max(box.width(), 1e-6);
    double h = std::max(box.height(), 1e-6);
    scale_ = std::min((viewWidth - 2 * marginScreen) / w, (viewHeight - 2 * marginScreen) / h);
    double centerX = (box.min.x + box.max.x) / 2.0;
    double centerY = (box.min.y + box.max.y) / 2.0;
    offset_.x = viewWidth / 2.0 - centerX * scale_;
    offset_.y = viewHeight / 2.0 + centerY * scale_;
}

// ---- Выбор и привязки ----

namespace {

bool selectable(const document::Document& document, const Entity& entity) {
    if (entity.layerId.isNil()) return true;
    const Layer* layer = document.findAs<Layer>(entity.layerId);
    return !layer || (layer->visible && !layer->locked);
}

double distanceToEntity(const Entity& entity, Point2 p) {
    if (auto* l = dynamic_cast<const LineEntity*>(&entity)) return geometry::distanceToSegment(p, l->start, l->end);
    if (auto* c = dynamic_cast<const CircleEntity*>(&entity)) return geometry::distanceToCircle(p, c->center, c->radius);
    if (auto* a = dynamic_cast<const ArcEntity*>(&entity))
        return geometry::distanceToArc(p, a->center, a->radius, a->startAngle, a->endAngle);
    if (auto* d = dynamic_cast<const LinearDimension*>(&entity)) {
        Point2 direction = d->p2 - d->p1;
        double len = geometry::length(direction);
        if (len < geometry::kLinearTolerance) return geometry::distance(p, d->p1);
        Point2 normal{-direction.y / len, direction.x / len};
        return geometry::distanceToSegment(p, d->p1 + normal * d->offset, d->p2 + normal * d->offset);
    }
    if (auto* rd = dynamic_cast<const RadialDimension*>(&entity)) {
        Point2 unit{std::cos(rd->angle), std::sin(rd->angle)};
        Point2 from = rd->diameter ? rd->center - unit * rd->radius : rd->center;
        return geometry::distanceToSegment(p, from, rd->center + unit * (rd->radius + rd->outsideExtension()));
    }
    if (auto* ad = dynamic_cast<const AngularDimension*>(&entity)) {
        double a1 = std::atan2(ad->p1.y - ad->vertex.y, ad->p1.x - ad->vertex.x);
        double a2 = std::atan2(ad->p2.y - ad->vertex.y, ad->p2.x - ad->vertex.x);
        return geometry::distanceToArc(p, ad->vertex, ad->arcRadius, a1, a2);
    }
    if (auto* ld = dynamic_cast<const Leader*>(&entity))
        return std::min(geometry::distanceToSegment(p, ld->tip, ld->knee),
                        geometry::distanceToSegment(p, ld->knee, ld->shelfEnd()));
    if (auto* h = dynamic_cast<const Hatch*>(&entity)) {
        // Штриховка выбирается щелчком по её линии, а не по любой точке внутри контура:
        // иначе она перехватывала бы выбор линий контура.
        double best = 1e300;
        for (const auto& [a, b] : h->strokes()) best = std::min(best, geometry::distanceToSegment(p, a, b));
        return best;
    }
    if (const auto parts = entity.primitives(); !parts.empty()) {
        // Составной объект выбирается щелчком по любой его линии или надписи.
        double best = 1e300;
        for (const auto& part : parts) {
            if (part.kind == drawing::Primitive::Kind::Line)
                best = std::min(best, geometry::distanceToSegment(p, part.a, part.b));
            else if (part.kind == drawing::Primitive::Kind::Text)
                best = std::min(best, geometry::distance(p, {part.a.x, part.a.y + part.textHeight / 2.0}) - part.textHeight);
            else
                best = std::min(best, std::abs(geometry::distance(p, part.a) - part.radius));
        }
        return std::max(best, 0.0);
    }
    BBox2 box = entity.bounds();
    if (box.contains(p, 0.0)) return 0.0;
    double dx = std::max({box.min.x - p.x, 0.0, p.x - box.max.x});
    double dy = std::max({box.min.y - p.y, 0.0, p.y - box.max.y});
    return std::hypot(dx, dy);
}

}  // namespace

std::optional<core::Uuid> hitTest(const document::Document& document, Point2 point, double tolerance) {
    std::optional<core::Uuid> best;
    double bestDistance = tolerance;
    for (const Entity* entity : document.objectsOf<Entity>()) {
        if (!selectable(document, *entity)) continue;
        double d = distanceToEntity(*entity, point);
        if (d <= bestDistance) {
            bestDistance = d;
            best = entity->id();
        }
    }
    return best;
}

std::vector<core::Uuid> entitiesInside(const document::Document& document, const BBox2& box) {
    std::vector<core::Uuid> ids;
    for (const Entity* entity : document.objectsOf<Entity>())
        if (selectable(document, *entity) && box.contains(entity->bounds(), 1e-9)) ids.push_back(entity->id());
    return ids;
}

std::optional<Snap> findSnap(const document::Document& document, Point2 point, double tolerance,
                             std::optional<Point2> from) {
    std::optional<Snap> best;
    double bestDistance = tolerance;
    auto consider = [&](Snap::Kind kind, Point2 candidate) {
        double d = geometry::distance(point, candidate);
        // При равном расстоянии концы и центры важнее середин.
        if (d < bestDistance - 1e-12 || (!best && d <= bestDistance)) {
            bestDistance = d;
            best = Snap{kind, candidate};
        }
    };

    std::vector<const LineEntity*> lines;
    for (const Entity* entity : document.objectsOf<Entity>()) {
        if (!selectable(document, *entity)) continue;
        if (auto* l = dynamic_cast<const LineEntity*>(entity)) {
            consider(Snap::Kind::Endpoint, l->start);
            consider(Snap::Kind::Endpoint, l->end);
            consider(Snap::Kind::Midpoint, geometry::midpoint(l->start, l->end));
            lines.push_back(l);
        } else if (auto* c = dynamic_cast<const CircleEntity*>(entity)) {
            consider(Snap::Kind::Center, c->center);
        } else if (auto* a = dynamic_cast<const ArcEntity*>(entity)) {
            consider(Snap::Kind::Center, a->center);
            consider(Snap::Kind::Endpoint, geometry::pointOnCircle(a->center, a->radius, a->startAngle));
            consider(Snap::Kind::Endpoint, geometry::pointOnCircle(a->center, a->radius, a->endAngle));
        } else {
            for (const Point2& p : entity->snapPoints()) consider(Snap::Kind::Endpoint, p);
        }
    }
    // Пересечения отрезков рядом с курсором.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (geometry::distanceToSegment(point, lines[i]->start, lines[i]->end) > tolerance) continue;
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            if (geometry::distanceToSegment(point, lines[j]->start, lines[j]->end) > tolerance) continue;
            if (auto x = geometry::segmentIntersection(lines[i]->start, lines[i]->end, lines[j]->start, lines[j]->end))
                consider(Snap::Kind::Intersection, *x);
        }
    }
    if (best) return best;

    // Основание перпендикуляра из точки from на отрезок рядом с курсором.
    if (from) {
        for (const LineEntity* l : lines) {
            Point2 direction = l->end - l->start;
            double lengthSquared = geometry::dot(direction, direction);
            if (lengthSquared < geometry::kLinearTolerance * geometry::kLinearTolerance) continue;
            double t = geometry::dot(*from - l->start, direction) / lengthSquared;
            if (t < 0.0 || t > 1.0) continue;
            Point2 foot = l->start + direction * t;
            if (geometry::distance(*from, foot) < geometry::kLinearTolerance) continue;  // from лежит на отрезке
            double d = geometry::distance(point, foot);
            if (d <= bestDistance) {
                bestDistance = d;
                best = Snap{Snap::Kind::Perpendicular, foot};
            }
        }
        if (best) return best;
    }

    // Ближайшая точка на отрезке, окружности или дуге.
    for (const Entity* entity : document.objectsOf<Entity>()) {
        if (!selectable(document, *entity)) continue;
        std::optional<Point2> candidate;
        if (auto* l = dynamic_cast<const LineEntity*>(entity)) {
            candidate = geometry::closestPointOnSegment(point, l->start, l->end);
        } else if (auto* c = dynamic_cast<const CircleEntity*>(entity)) {
            double d = geometry::distance(point, c->center);
            if (d > geometry::kLinearTolerance) candidate = c->center + (point - c->center) * (c->radius / d);
        } else if (auto* a = dynamic_cast<const ArcEntity*>(entity)) {
            double angle = std::atan2(point.y - a->center.y, point.x - a->center.x);
            if (geometry::angleOnArc(angle, a->startAngle, a->endAngle))
                candidate = geometry::pointOnCircle(a->center, a->radius, angle);
        }
        if (!candidate) continue;
        double d = geometry::distance(point, *candidate);
        if (d <= bestDistance) {
            bestDistance = d;
            best = Snap{Snap::Kind::Nearest, *candidate};
        }
    }
    return best;
}

}  // namespace cosmocad::graphics
