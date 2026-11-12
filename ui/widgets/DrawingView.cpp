#include "DrawingView.h"

#include <cstddef>
#include <utility>
#include <vector>
#include <optional>
#include <map>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLocale>
#include <QPainter>
#include <QRegularExpression>
#include <QWheelEvent>
#include <cmath>

#include "ScenePainter.h"
#include "cosmocad/core/Units.h"

namespace cosmocad::ui {

using geometry::Point2;
using geometry::Transform2;

namespace {
constexpr double kPickPixels = 8.0;  // радиус выбора и привязки в точках экрана
constexpr double kDragPixels = 4.0;  // с какого смещения щелчок считается рамкой
}  // namespace

DrawingView::DrawingView(app::AppContext& context, QWidget* parent) : QWidget(parent), context_(context) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(400, 300);
}

void DrawingView::setDocument(document::Document* document) {
    if (document_) document_->changed.disconnect(connection_);
    document_ = document;
    selection_.clear();
    currentLayer_ = {};
    resetInput();
    lastPoint_.reset();
    fitted_ = false;
    sceneDirty_ = true;
    if (document_) {
        connection_ = document_->changed.connect([this] {
            sceneDirty_ = true;
            // Удалённые объекты (в том числе отменой) не должны оставаться выбранными.
            std::erase_if(selection_, [this](const core::Uuid& id) { return !document_->find(id); });
            update();
            emit documentChanged();
        });
    }
    fitAll();
    emit selectionChanged();
    emit documentChanged();
}

void DrawingView::setTool(Tool tool) {
    tool_ = tool;
    resetInput();
    setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    if (needsSelection() && selection_.empty()) emit message(tr("Сначала выберите элементы инструментом «Выбор»"));
    update();
}

const graphics::Scene& DrawingView::currentScene() const {
    if (sceneDirty_) {
        scene_ = {};
        if (document_) {
            auto profile = context_.profileFor(*document_);
            scene_ = graphics::buildScene(*document_, profile ? &*profile : nullptr);
        }
        sceneDirty_ = false;
    }
    return scene_;
}

void DrawingView::fitAll() {
    const graphics::Scene& scene = currentScene();
    if (scene.bounds.valid && width() > 0 && height() > 0) {
        view_.fit(scene.bounds, width(), height());
        fitted_ = true;
    }
    update();
}

void DrawingView::select(const core::Uuid& id) {
    selection_.clear();
    if (!id.isNil()) selection_.insert(id);
    emit selectionChanged();
    update();
}

void DrawingView::selectMany(const std::vector<core::Uuid>& ids) {
    selection_.clear();
    for (const auto& id : ids)
        if (!id.isNil()) selection_.insert(id);
    emit selectionChanged();
    update();
}

std::vector<DrawingView::ConstraintGlyph> DrawingView::constraintGlyphs() const {
    using sketcher::ConstraintKind;
    std::vector<ConstraintGlyph> out;
    if (!document_) return out;
    // Где у элемента ставить значок: середина отрезка, центр окружности.
    const auto anchor = [&](const sketcher::Ref& ref) -> std::optional<Point2> {
        if (const auto* line = document_->findAs<drawing::LineEntity>(ref.entityId)) {
            if (ref.point == sketcher::PointRole::Start) return line->start;
            if (ref.point == sketcher::PointRole::End) return line->end;
            return geometry::midpoint(line->start, line->end);
        }
        if (const auto* circle = document_->findAs<drawing::CircleEntity>(ref.entityId)) return circle->center;
        if (const auto* arc = document_->findAs<drawing::ArcEntity>(ref.entityId)) return arc->center;
        return std::nullopt;
    };
    const QLocale russian(QLocale::Russian);
    for (const auto* constraint : document_->objectsOf<sketcher::Constraint>()) {
        QString text;
        bool onEach = true;  // значок у каждого элемента ограничения; иначе — один, у первого
        switch (constraint->kind) {
            case ConstraintKind::Fixed: text = QStringLiteral("⚓"); break;
            case ConstraintKind::Coincident: text = QStringLiteral("●"); onEach = false; break;
            case ConstraintKind::Horizontal: text = QStringLiteral("—"); break;
            case ConstraintKind::Vertical: text = QStringLiteral("|"); break;
            case ConstraintKind::Parallel: text = QStringLiteral("∥"); break;
            case ConstraintKind::Perpendicular: text = QStringLiteral("⊥"); break;
            case ConstraintKind::EqualLength:
            case ConstraintKind::EqualRadius: text = QStringLiteral("="); break;
            case ConstraintKind::Concentric: text = QStringLiteral("◎"); onEach = false; break;
            case ConstraintKind::Tangent: text = QStringLiteral("⌒"); onEach = false; break;
            case ConstraintKind::PointOnLine: text = QStringLiteral("∈"); onEach = false; break;
            case ConstraintKind::Symmetric: text = QStringLiteral("⇔"); break;
            case ConstraintKind::Length:
            case ConstraintKind::Distance: text = russian.toString(constraint->value, 'g', 8); onEach = false; break;
            case ConstraintKind::Radius: text = QStringLiteral("R") + russian.toString(constraint->value, 'g', 8); onEach = false; break;
            case ConstraintKind::Angle: text = russian.toString(constraint->value, 'g', 8) + QStringLiteral("°"); onEach = false; break;
        }
        // Размер, заданный формулой, показывает и формулу.
        if (!constraint->expression.empty()) text += QStringLiteral(" (") + QString::fromStdString(constraint->expression) + QStringLiteral(")");
        for (const auto& ref : constraint->refs) {
            if (const auto at = anchor(ref)) out.push_back({*at, text});
            if (!onEach) break;
        }
    }
    return out;
}

void DrawingView::selectAll() {
    if (!document_) return;
    selection_.clear();
    // Скрытые и заблокированные слои отсекает entitiesInside.
    geometry::BBox2 everything = geometry::BBox2::fromCorners({-1e12, -1e12}, {1e12, 1e12});
    for (const auto& id : graphics::entitiesInside(*document_, everything)) selection_.insert(id);
    emit selectionChanged();
    update();
}

void DrawingView::clearSelection() {
    selection_.clear();
    emit selectionChanged();
    update();
}

void DrawingView::deleteSelection() {
    if (!document_ || selection_.empty()) return;
    auto ids = selectedIds();
    if (auto removed = context_.drawing().removeObjects(*document_, ids); !removed) return report(removed.error());
    selection_.clear();
    emit selectionChanged();
    emit documentEdited();
}

QPointF DrawingView::toScreen(Point2 world) const {
    Point2 s = view_.toScreen(world);
    return {s.x, s.y};
}

Point2 DrawingView::worldAt(const QPointF& position) const { return view_.toWorld({position.x(), position.y()}); }

Point2 DrawingView::snapped(const QPointF& position) {
    Point2 world = worldAt(position);
    snap_.reset();
    if (document_) {
        std::optional<Point2> from;
        if (!points_.empty()) from = points_.back();
        snap_ = graphics::findSnap(*document_, world, kPickPixels / view_.scale(), from);
    }
    return snap_ ? snap_->point : world;
}

bool DrawingView::needsSelection() const {
    return tool_ == Tool::Move || tool_ == Tool::Copy || tool_ == Tool::Rotate || tool_ == Tool::Mirror;
}

// Инструменты, которым нужен щелчок по самому элементу, а не точная точка привязки.
bool DrawingView::picksEntities() const {
    if (tool_ == Tool::Trim || tool_ == Tool::Fillet || tool_ == Tool::Chamfer) return true;
    return (tool_ == Tool::Radius || tool_ == Tool::Diameter) && points_.empty();
}

std::optional<core::Uuid> DrawingView::pick(Point2 point) const {
    if (!document_) return std::nullopt;
    return graphics::hitTest(*document_, point, kPickPixels / view_.scale());
}

bool DrawingView::placeText(Point2 position, const QString& text, double height) {
    if (!document_) return false;
    return finish(context_.drawing().addText(*document_, position, text.toStdString(), height, layerForNew()));
}

bool DrawingView::placeLeader(Point2 tip, Point2 knee, const QString& text, double height) {
    if (!document_) return false;
    return finish(context_.drawing().addLeader(*document_, tip, knee, text.toStdString(), height, layerForNew()));
}

bool DrawingView::placeRoughness(Point2 at, int kind, const QString& parameter, int quarterTurns) {
    if (!document_) return false;
    drawing::RoughnessParams params;
    params.position = at;
    params.kind = kind == 1 ? drawing::RoughnessSign::Kind::Removal
                  : kind == 2 ? drawing::RoughnessSign::Kind::NoRemoval : drawing::RoughnessSign::Kind::Any;
    params.parameter = parameter.trimmed().toStdString();
    params.quarterTurns = quarterTurns;
    params.layer = layerForNew();
    return finish(context_.drawing().addRoughness(*document_, params));
}

bool DrawingView::hatchSelection(double angleDegrees, double spacing) {
    if (!document_) return false;
    if (selection_.empty()) {
        emit message(tr("Сначала выберите линии, ограничивающие область"));
        return false;
    }
    return finish(context_.drawing().addHatch(*document_, selectedIds(), core::degreesToRadians(angleDegrees), spacing,
                                              layerForNew()));
}

core::Uuid DrawingView::layerForNew() const {
    if (!currentLayer_.isNil() && document_ && document_->findAs<drawing::Layer>(currentLayer_)) return currentLayer_;
    return {};
}

std::vector<core::Uuid> DrawingView::selectedIds() const {
    // Порядок — как в документе, чтобы результат не зависел от порядка выбора.
    std::vector<core::Uuid> ids;
    if (!document_) return ids;
    for (const auto& object : document_->objects())
        if (selection_.count(object->id())) ids.push_back(object->id());
    return ids;
}

void DrawingView::resetInput() {
    points_.clear();
    picked_.clear();
    snap_.reset();
    dragging_ = false;
}

void DrawingView::report(const core::Error& error) { emit message(QString::fromStdString(error.message)); }

template <class T>
bool DrawingView::finish(const core::Result<T>& result) {
    if (!result) {
        report(result.error());
        return false;
    }
    emit documentEdited();
    return true;
}

// Единая точка входа для щелчка мышью и ввода координат с клавиатуры.
void DrawingView::acceptPoint(Point2 point) {
    if (!document_ || tool_ == Tool::Select) return;
    if (needsSelection() && selection_.empty()) {
        emit message(tr("Сначала выберите элементы инструментом «Выбор»"));
        return;
    }
    auto& service = context_.drawing();
    lastPoint_ = point;
    points_.push_back(point);

    switch (tool_) {
        case Tool::Select:
            break;
        case Tool::Line:
            if (points_.size() == 2) {
                bool ok = finish(service.addLine(*document_, points_[0], points_[1], layerForNew()));
                Point2 next = points_[1];
                points_.clear();
                if (ok) points_.push_back(next);  // следующий отрезок продолжается от конца
            }
            break;
        case Tool::Rectangle:
            if (points_.size() == 2) {
                finish(service.addRectangle(*document_, points_[0], points_[1], layerForNew()));
                points_.clear();
            }
            break;
        case Tool::Circle:
            if (points_.size() == 2) {
                finish(service.addCircle(*document_, points_[0], geometry::distance(points_[0], points_[1]), layerForNew()));
                points_.clear();
            }
            break;
        case Tool::Arc:
            if (points_.size() == 3) {
                Point2 c = points_[0];
                double radius = geometry::distance(c, points_[1]);
                double start = std::atan2(points_[1].y - c.y, points_[1].x - c.x);
                double end = std::atan2(points_[2].y - c.y, points_[2].x - c.x);
                finish(service.addArc(*document_, c, radius, start, end, layerForNew()));
                points_.clear();
            }
            break;
        case Tool::SchElement:
            emit schematicElementRequested(point.x, point.y);
            points_.clear();
            break;
        case Tool::SchLabel:
            emit schematicLabelRequested(point.x, point.y);
            points_.clear();
            break;
        case Tool::Roughness:
            emit roughnessRequested(point.x, point.y);
            points_.clear();
            break;
        case Tool::SchWire:
            if (points_.size() == 2) {
                const geometry::Point2 last = points_[1];
                emit schematicWireRequested(points_[0].x, points_[0].y, last.x, last.y);
                points_.clear();
                points_.push_back(last);  // следующий участок начинается там, где кончился этот
            }
            break;
        case Tool::Text:
            emit textRequested(point.x, point.y);
            points_.clear();
            break;
        case Tool::Leader:
            if (points_.size() == 2) {
                emit leaderRequested(points_[0].x, points_[0].y, points_[1].x, points_[1].y);
                points_.clear();
            }
            break;
        case Tool::Radius:
        case Tool::Diameter:
            if (points_.size() == 1) {
                // Первый щелчок — по окружности или дуге.
                auto id = pick(point);
                const auto* circle = id ? document_->findAs<drawing::CircleEntity>(*id) : nullptr;
                const auto* arc = id ? document_->findAs<drawing::ArcEntity>(*id) : nullptr;
                if (!circle && !arc) {
                    emit message(tr("Укажите окружность или дугу"));
                    points_.clear();
                } else {
                    picked_ = {*id};
                }
            } else if (points_.size() == 2 && !picked_.empty()) {
                const auto* circle = document_->findAs<drawing::CircleEntity>(picked_[0]);
                const auto* arc = document_->findAs<drawing::ArcEntity>(picked_[0]);
                if (circle || arc) {
                    Point2 c = circle ? circle->center : arc->center;
                    double r = circle ? circle->radius : arc->radius;
                    double angle = std::atan2(points_[1].y - c.y, points_[1].x - c.x);
                    finish(service.addRadialDimension(*document_, c, r, angle, tool_ == Tool::Diameter, layerForNew()));
                }
                resetInput();
            }
            break;
        case Tool::Angle:
            if (points_.size() == 4) {
                double radius = geometry::distance(points_[0], points_[3]);
                finish(service.addAngularDimension(*document_, points_[0], points_[1], points_[2], radius, layerForNew()));
                points_.clear();
            }
            break;
        case Tool::Trim: {
            auto id = pick(point);
            if (!id || !document_->findAs<drawing::LineEntity>(*id)) emit message(tr("Укажите отрезок, часть которого нужно убрать"));
            else finish(service.trimLine(*document_, *id, point));
            points_.clear();
            break;
        }
        case Tool::Fillet:
        case Tool::Chamfer: {
            auto id = pick(point);
            if (!id || !document_->findAs<drawing::LineEntity>(*id)) {
                emit message(tr("Укажите отрезок"));
                points_.pop_back();
                break;
            }
            picked_.push_back(*id);
            if (picked_.size() == 2) {
                if (tool_ == Tool::Fillet)
                    finish(service.filletLines(*document_, picked_[0], points_[0], picked_[1], points_[1], cornerSize_));
                else
                    finish(service.chamferLines(*document_, picked_[0], points_[0], picked_[1], points_[1], cornerSize_));
                resetInput();
            }
            break;
        }
        case Tool::Dimension:
            if (points_.size() == 3) {
                Point2 direction = points_[1] - points_[0];
                double len = geometry::length(direction);
                double offset = len > 0.0 ? geometry::cross(direction, points_[2] - points_[0]) / len : 10.0;
                finish(service.addLinearDimension(*document_, points_[0], points_[1], offset, layerForNew()));
                points_.clear();
            }
            break;
        case Tool::Move:
            if (points_.size() == 2) {
                finish(service.transformEntities(*document_, selectedIds(),
                                                 Transform2::translation(points_[1] - points_[0]), "Переместить"));
                points_.clear();
            }
            break;
        case Tool::Copy:
            if (points_.size() == 2) {
                finish(service.copyEntities(*document_, selectedIds(), Transform2::translation(points_[1] - points_[0])));
                points_.resize(1);  // можно поставить ещё одну копию от той же базовой точки
            }
            break;
        case Tool::Rotate:
            if (points_.size() == 2) {
                double angle = std::atan2(points_[1].y - points_[0].y, points_[1].x - points_[0].x);
                finish(service.transformEntities(*document_, selectedIds(), Transform2::rotation(points_[0], angle),
                                                 "Повернуть"));
                points_.clear();
            }
            break;
        case Tool::Mirror:
            if (points_.size() == 2) {
                if (geometry::distance(points_[0], points_[1]) < geometry::kLinearTolerance) {
                    emit message(tr("Точки оси отражения совпадают"));
                } else {
                    finish(service.transformEntities(*document_, selectedIds(),
                                                     Transform2::mirror(points_[0], points_[1]), "Отразить"));
                }
                points_.clear();
            }
            break;
    }
    update();
}

// Одно число: радиус окружности или угол поворота.
bool DrawingView::acceptValue(double value) {
    if (!document_) return false;
    if ((tool_ == Tool::Fillet || tool_ == Tool::Chamfer) && points_.empty()) {
        if (value < 0.0) return false;
        cornerSize_ = value;
        emit message(tool_ == Tool::Fillet ? tr("Радиус скругления: %1 мм").arg(value) : tr("Размер фаски: %1 мм").arg(value));
        return true;
    }
    if (points_.size() != 1) return false;
    auto& service = context_.drawing();
    if (tool_ == Tool::Circle) {
        finish(service.addCircle(*document_, points_[0], value, layerForNew()));
        points_.clear();
        update();
        return true;
    }
    if (tool_ == Tool::Rotate && !selection_.empty()) {
        finish(service.transformEntities(*document_, selectedIds(),
                                         Transform2::rotation(points_[0], core::degreesToRadians(value)), "Повернуть"));
        points_.clear();
        update();
        return true;
    }
    return false;
}

bool DrawingView::inputText(const QString& text) {
    QString cleaned = text.trimmed();
    if (cleaned.isEmpty() || !document_ || tool_ == Tool::Select) return false;
    bool relative = cleaned.startsWith('@');
    if (relative) cleaned.remove(0, 1);
    cleaned.replace(',', '.');  // десятичная запятая
    const QStringList parts = cleaned.split(QRegularExpression(QStringLiteral("[\\s;]+")), Qt::SkipEmptyParts);

    bool okX = false, okY = false;
    if (parts.size() == 1 && !relative) {
        double value = parts[0].toDouble(&okX);
        if (okX && std::isfinite(value) && acceptValue(value)) return true;
        emit message(tr("Введите два числа: X и Y"));
        return false;
    }
    if (parts.size() != 2) {
        emit message(tr("Введите два числа: X и Y"));
        return false;
    }
    Point2 point{parts[0].toDouble(&okX), parts[1].toDouble(&okY)};
    if (!okX || !okY || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        emit message(tr("Не удалось разобрать числа"));
        return false;
    }
    if (relative) {
        if (!lastPoint_) {
            emit message(tr("Нет предыдущей точки для смещения"));
            return false;
        }
        point = *lastPoint_ + point;
    }
    acceptPoint(point);
    return true;
}

void DrawingView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0xF7, 0xF9, 0xFC));
    if (!document_) {
        painter.drawText(rect(), Qt::AlignCenter, tr("Нет открытого чертежа"));
        return;
    }
    const graphics::Scene& scene = currentScene();
    if (!fitted_ && scene.bounds.valid) {
        view_.fit(scene.bounds, width(), height());
        fitted_ = true;
    }
    if (scene.sheetWidth > 0.0) {
        QPointF topLeft = toScreen({0.0, scene.sheetHeight});
        painter.fillRect(QRectF(topLeft.x(), topLeft.y(), scene.sheetWidth * view_.scale(), scene.sheetHeight * view_.scale()),
                         Qt::white);
    }

    ScenePaintOptions options;
    options.scale = view_.scale();
    options.origin = toScreen({0.0, 0.0});
    options.selection = &selection_;
    paintScene(painter, scene, options);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    // Значки ограничений эскиза — зелёные пометки рядом с элементами. Несколько значков в одной
    // точке раскладываются в строку.
    {
        std::map<std::pair<long, long>, int> stacked;
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        for (const auto& glyph : constraintGlyphs()) {
            const QPointF at = toScreen(glyph.at);
            int& used = stacked[{std::lround(at.x()), std::lround(at.y())}];
            const int width = painter.fontMetrics().horizontalAdvance(glyph.text) + 6;
            const QRectF box(at.x() + 6 + used, at.y() - 20, width, 15);
            used += width + 2;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(232, 245, 233));
            painter.drawRoundedRect(box, 3, 3);
            painter.setPen(QColor(27, 110, 60));
            painter.drawText(box, Qt::AlignCenter, glyph.text);
        }
        painter.setBrush(Qt::NoBrush);
    }

    // Рамка выбора.
    if (dragging_) {
        painter.setPen(QPen(QColor(0, 110, 230), 1, Qt::DashLine));
        painter.drawRect(QRectF(pressPosition_, lastMouse_).normalized());
    }

    // Предварительный показ строящегося элемента.
    painter.setPen(QPen(QColor(0, 110, 230), 1, Qt::DashLine));
    if (!points_.empty()) {
        QPointF first = toScreen(points_[0]);
        QPointF cursor = toScreen(cursor_);
        switch (tool_) {
            case Tool::Line:
            case Tool::SchWire:
            case Tool::Leader:
            case Tool::Move:
            case Tool::Copy:
            case Tool::Rotate:
            case Tool::Mirror:
                painter.drawLine(first, cursor);
                break;
            case Tool::Rectangle:
                painter.drawRect(QRectF(first, cursor).normalized());
                break;
            case Tool::Circle: {
                double r = geometry::distance(points_[0], cursor_) * view_.scale();
                painter.drawEllipse(first, r, r);
                break;
            }
            case Tool::Arc:
                painter.drawLine(first, points_.size() > 1 ? toScreen(points_[1]) : cursor);
                if (points_.size() > 1) painter.drawLine(first, cursor);
                break;
            case Tool::Dimension:
                if (points_.size() == 1) {
                    painter.drawLine(first, cursor);
                } else {
                    painter.drawLine(first, toScreen(points_[1]));
                    painter.drawLine(toScreen(geometry::midpoint(points_[0], points_[1])), cursor);
                }
                break;
            case Tool::Angle:
                for (std::size_t i = 1; i < points_.size() && i < 3; ++i) painter.drawLine(first, toScreen(points_[i]));
                painter.drawLine(first, cursor);
                break;
            case Tool::Radius:
            case Tool::Diameter:
                if (!picked_.empty()) {
                    if (const auto* circle = document_->findAs<drawing::CircleEntity>(picked_[0]))
                        painter.drawLine(toScreen(circle->center), cursor);
                    else if (const auto* arc = document_->findAs<drawing::ArcEntity>(picked_[0]))
                        painter.drawLine(toScreen(arc->center), cursor);
                }
                break;
            case Tool::Select:
            case Tool::SchElement:
            case Tool::SchLabel:
            case Tool::Roughness:
            case Tool::Text:
            case Tool::Trim:
            case Tool::Fillet:
            case Tool::Chamfer:
                break;
        }
    }

    // Значок привязки.
    if (snap_ && tool_ != Tool::Select && !picksEntities()) {
        QPointF s = toScreen(snap_->point);
        painter.setPen(QPen(QColor(230, 120, 0), 2));
        switch (snap_->kind) {
            case graphics::Snap::Kind::Endpoint: painter.drawRect(QRectF(s.x() - 5, s.y() - 5, 10, 10)); break;
            case graphics::Snap::Kind::Midpoint:
                painter.drawPolygon(QPolygonF({QPointF(s.x(), s.y() - 6), QPointF(s.x() - 6, s.y() + 5),
                                               QPointF(s.x() + 6, s.y() + 5)}));
                break;
            case graphics::Snap::Kind::Center: painter.drawEllipse(s, 5, 5); break;
            case graphics::Snap::Kind::Intersection:
                painter.drawLine(QPointF(s.x() - 5, s.y() - 5), QPointF(s.x() + 5, s.y() + 5));
                painter.drawLine(QPointF(s.x() - 5, s.y() + 5), QPointF(s.x() + 5, s.y() - 5));
                break;
            case graphics::Snap::Kind::Perpendicular:
                painter.drawLine(QPointF(s.x() - 6, s.y() + 6), QPointF(s.x() + 6, s.y() + 6));
                painter.drawLine(QPointF(s.x(), s.y() + 6), QPointF(s.x(), s.y() - 6));
                break;
            case graphics::Snap::Kind::Nearest:
                painter.drawPolygon(QPolygonF({QPointF(s.x() - 5, s.y() - 5), QPointF(s.x() + 5, s.y() - 5),
                                               QPointF(s.x() - 5, s.y() + 5), QPointF(s.x() + 5, s.y() + 5)}));
                break;
        }
    }
}

void DrawingView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!fitted_) fitAll();
}

void DrawingView::zoomBy(double factor) {
    view_.zoomAt({width() / 2.0, height() / 2.0}, factor);
    update();
}

void DrawingView::wheelEvent(QWheelEvent* event) {
    double factor = std::pow(1.0015, event->angleDelta().y());
    view_.zoomAt({event->position().x(), event->position().y()}, factor);
    update();
}

void DrawingView::mousePressEvent(QMouseEvent* event) {
    lastMouse_ = event->position();
    pressPosition_ = event->position();
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        panning_ = true;
        return;
    }
    if (event->button() != Qt::LeftButton || !document_) return;
    if (tool_ == Tool::Select) return;  // выбор завершается при отпускании кнопки
    acceptPoint(picksEntities() ? worldAt(event->position()) : snapped(event->position()));
}

void DrawingView::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        QPointF delta = event->position() - lastMouse_;
        view_.pan(delta.x(), delta.y());
        lastMouse_ = event->position();
        update();
        return;
    }
    lastMouse_ = event->position();
    if (tool_ == Tool::Select) {
        cursor_ = worldAt(event->position());
        if ((event->buttons() & Qt::LeftButton) &&
            (event->position() - pressPosition_).manhattanLength() > kDragPixels) {
            dragging_ = true;
            update();
        }
    } else {
        cursor_ = snapped(event->position());
        update();
    }
    emit cursorMoved(QStringLiteral("X: %1  Y: %2 мм").arg(cursor_.x, 0, 'f', 2).arg(cursor_.y, 0, 'f', 2));
}

void DrawingView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        panning_ = false;
        return;
    }
    if (event->button() != Qt::LeftButton || !document_ || tool_ != Tool::Select) return;

    bool add = event->modifiers() & Qt::ShiftModifier;
    if (!add) selection_.clear();
    // Рамка — если кнопку отпустили заметно в стороне от места нажатия.
    if (dragging_ || (event->position() - pressPosition_).manhattanLength() > kDragPixels) {
        auto box = geometry::BBox2::fromCorners(worldAt(pressPosition_), worldAt(event->position()));
        for (const auto& id : graphics::entitiesInside(*document_, box)) selection_.insert(id);
        dragging_ = false;
    } else if (auto hit = graphics::hitTest(*document_, worldAt(event->position()), kPickPixels / view_.scale())) {
        selection_.insert(*hit);
    }
    emit selectionChanged();
    update();
}

void DrawingView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        resetInput();
        update();
    } else if (event->key() == Qt::Key_Delete) {
        deleteSelection();
    } else {
        QWidget::keyPressEvent(event);
    }
}

}  // namespace cosmocad::ui
