// Тесты геометрии, сцены, выбора и привязок (требования 2D-3, ПЛ-3, ПЛ-4).
#include <gtest/gtest.h>
#include <numbers>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/IDrawingService.h"
#include "cosmocad/geometry/Geometry2D.h"
#include "cosmocad/graphics/Scene.h"

using namespace cosmocad;
using geometry::Point2;
const double kPi = std::numbers::pi;

TEST(Geometry, DistanceToSegmentCoversEndsAndInterior) {
    EXPECT_NEAR(geometry::distanceToSegment({5, 3}, {0, 0}, {10, 0}), 3.0, 1e-12);
    EXPECT_NEAR(geometry::distanceToSegment({-3, 4}, {0, 0}, {10, 0}), 5.0, 1e-12);
    EXPECT_NEAR(geometry::distanceToSegment({13, 4}, {0, 0}, {10, 0}), 5.0, 1e-12);
    EXPECT_NEAR(geometry::distanceToSegment({3, 4}, {0, 0}, {0, 0}), 5.0, 1e-12);  // вырожденный отрезок
}

TEST(Geometry, AngleOnArcHandlesWrapAround) {
    EXPECT_TRUE(geometry::angleOnArc(kPi / 4, 0.0, kPi / 2));
    EXPECT_FALSE(geometry::angleOnArc(kPi, 0.0, kPi / 2));
    // Дуга через ноль: от 315° до 45°.
    EXPECT_TRUE(geometry::angleOnArc(0.0, 7 * kPi / 4, kPi / 4));
    EXPECT_FALSE(geometry::angleOnArc(kPi, 7 * kPi / 4, kPi / 4));
    EXPECT_NEAR(geometry::normalizeAngle(-kPi / 2), 3 * kPi / 2, 1e-12);
}

TEST(Geometry, ArcBoundsIncludeQuadrantPoints) {
    // Четверть окружности от 0° до 90°: габарит — квадрат [0..r] × [0..r].
    auto quarter = geometry::arcBounds({0, 0}, 10.0, 0.0, kPi / 2);
    EXPECT_NEAR(quarter.min.x, 0.0, 1e-9);
    EXPECT_NEAR(quarter.max.x, 10.0, 1e-9);
    EXPECT_NEAR(quarter.max.y, 10.0, 1e-9);
    // Дуга от 45° до 135° проходит через верхнюю точку.
    auto top = geometry::arcBounds({0, 0}, 10.0, kPi / 4, 3 * kPi / 4);
    EXPECT_NEAR(top.max.y, 10.0, 1e-9);
    EXPECT_NEAR(top.min.y, 10.0 * std::sin(kPi / 4), 1e-9);
}

TEST(Geometry, SegmentIntersection) {
    auto x = geometry::segmentIntersection({0, 0}, {10, 10}, {0, 10}, {10, 0});
    ASSERT_TRUE(x.has_value());
    EXPECT_NEAR(x->x, 5.0, 1e-12);
    EXPECT_NEAR(x->y, 5.0, 1e-12);
    EXPECT_FALSE(geometry::segmentIntersection({0, 0}, {10, 0}, {0, 1}, {10, 1}).has_value());  // параллельны
    EXPECT_FALSE(geometry::segmentIntersection({0, 0}, {1, 1}, {5, 0}, {0, 5}).has_value());    // не достают
}

TEST(ViewTransform, ZoomKeepsPointUnderCursorAndRoundTrips) {
    graphics::ViewTransform2D view;
    view.fit(geometry::BBox2::fromCorners({0, 0}, {210, 297}), 800, 600);
    Point2 screen{321.0, 123.0};
    Point2 before = view.toWorld(screen);
    view.zoomAt(screen, 3.7);
    Point2 after = view.toWorld(screen);
    EXPECT_NEAR(before.x, after.x, 1e-9);
    EXPECT_NEAR(before.y, after.y, 1e-9);
    Point2 world{55.5, 170.25};
    Point2 back = view.toWorld(view.toScreen(world));
    EXPECT_NEAR(back.x, world.x, 1e-9);
    EXPECT_NEAR(back.y, world.y, 1e-9);
}

TEST(ViewTransform, FitShowsWholeBoxInsideView) {
    graphics::ViewTransform2D view;
    auto box = geometry::BBox2::fromCorners({0, 0}, {210, 297});
    view.fit(box, 800, 600, 20);
    Point2 a = view.toScreen(box.min), b = view.toScreen(box.max);
    EXPECT_GE(std::min(a.x, b.x), 20 - 1e-6);
    EXPECT_LE(std::max(a.x, b.x), 780 + 1e-6);
    EXPECT_GE(std::min(a.y, b.y), 20 - 1e-6);
    EXPECT_LE(std::max(a.y, b.y), 580 + 1e-6);
    EXPECT_GT(a.y, b.y);  // ось Y экрана направлена вниз
}

class SelectionTest : public ::testing::Test {
protected:
    document::Document doc{document::DocumentKind::Drawing, "t"};
    drawing::DrawingService impl;
    drawing::IDrawingService& service = impl;  // значения по умолчанию заданы в интерфейсе
};

TEST_F(SelectionTest, HitTestPicksNearestAndRespectsTolerance) {
    auto line = service.addLine(doc, {0, 0}, {100, 0}).value();
    auto circle = service.addCircle(doc, {50, 50}, 20).value();
    EXPECT_EQ(graphics::hitTest(doc, {50, 1}, 2.0), line);
    EXPECT_EQ(graphics::hitTest(doc, {50, 71}, 2.0), circle);  // рядом с окружностью
    EXPECT_FALSE(graphics::hitTest(doc, {50, 50}, 2.0).has_value());  // центр — не линия окружности
    EXPECT_FALSE(graphics::hitTest(doc, {50, 10}, 2.0).has_value());
}

TEST_F(SelectionTest, HiddenAndLockedLayersAreNotSelectable) {
    auto layer = service.addLayer(doc, "Скрытый", "solid_main").value();
    service.addLine(doc, {0, 0}, {100, 0}, layer).value();
    EXPECT_TRUE(graphics::hitTest(doc, {50, 0}, 1.0).has_value());

    document::Transaction tx(doc, "hide");
    ASSERT_TRUE(tx.modifyAs<drawing::Layer>(layer, [](drawing::Layer& l) { l.visible = false; }).ok());
    tx.commit();
    EXPECT_FALSE(graphics::hitTest(doc, {50, 0}, 1.0).has_value());
    EXPECT_TRUE(graphics::buildScene(doc, nullptr).items.empty());  // скрытый слой не рисуется и не печатается

    document::Transaction tx2(doc, "lock");
    ASSERT_TRUE(tx2.modifyAs<drawing::Layer>(layer, [](drawing::Layer& l) {
                       l.visible = true;
                       l.locked = true;
                   }).ok());
    tx2.commit();
    EXPECT_FALSE(graphics::hitTest(doc, {50, 0}, 1.0).has_value());
    EXPECT_EQ(graphics::buildScene(doc, nullptr).items.size(), 1u);  // заблокированный слой виден
}

TEST_F(SelectionTest, SnapFindsExactPoints) {
    service.addLine(doc, {0, 0}, {100, 0}).value();
    service.addLine(doc, {50, -50}, {50, 50}).value();
    service.addCircle(doc, {200, 200}, 30).value();

    auto end = graphics::findSnap(doc, {99.2, 0.7}, 2.0);
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end->kind, graphics::Snap::Kind::Endpoint);
    EXPECT_EQ(end->point, (Point2{100, 0}));  // точное совпадение, без погрешности курсора

    auto center = graphics::findSnap(doc, {201, 199}, 2.0);
    ASSERT_TRUE(center.has_value());
    EXPECT_EQ(center->kind, graphics::Snap::Kind::Center);
    EXPECT_EQ(center->point, (Point2{200, 200}));

    service.addLine(doc, {0, 300}, {40, 300}).value();
    auto mid = graphics::findSnap(doc, {20.5, 300.4}, 2.0);
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->kind, graphics::Snap::Kind::Midpoint);
    EXPECT_NEAR(mid->point.x, 20.0, 1e-12);
    EXPECT_NEAR(mid->point.y, 300.0, 1e-12);

    EXPECT_FALSE(graphics::findSnap(doc, {300, 300}, 2.0).has_value());
}

TEST_F(SelectionTest, SnapFindsIntersection) {
    service.addLine(doc, {0, 0}, {100, 100}).value();
    service.addLine(doc, {0, 60}, {100, 0}).value();
    // Пересечение: y = x и y = 60 - 0.6x → x = 37.5.
    auto snap = graphics::findSnap(doc, {37.9, 37.2}, 1.5);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->kind, graphics::Snap::Kind::Intersection);
    EXPECT_NEAR(snap->point.x, 37.5, 1e-9);
    EXPECT_NEAR(snap->point.y, 37.5, 1e-9);
}

TEST_F(SelectionTest, DrawingServiceRejectsDegenerateInput) {
    EXPECT_FALSE(service.addLine(doc, {1, 1}, {1, 1}).ok());
    EXPECT_FALSE(service.addCircle(doc, {0, 0}, 0.0).ok());
    EXPECT_FALSE(service.addCircle(doc, {0, 0}, -5.0).ok());
    EXPECT_FALSE(service.addLine(doc, {0, 0}, {std::nan(""), 1}).ok());
    EXPECT_FALSE(service.addLine(doc, {0, 0}, {1, 1}, core::Uuid::generate()).ok());  // несуществующий слой
    EXPECT_EQ(doc.size(), 0u);
    EXPECT_FALSE(doc.canUndo());
}

TEST_F(SelectionTest, MoveIsOneUndoStepAndFailsAtomically) {
    auto a = service.addLine(doc, {0, 0}, {10, 0}).value();
    auto b = service.addCircle(doc, {5, 5}, 2).value();
    ASSERT_TRUE(service.moveEntities(doc, {a, b}, {100, 50}).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(a)->start.x, 100.0);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::CircleEntity>(b)->center.y, 55.0);
    ASSERT_TRUE(doc.undo());  // один шаг отмены возвращает оба элемента
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(a)->start.x, 0.0);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::CircleEntity>(b)->center.y, 5.0);

    // Один из номеров не существует — не меняется ничего.
    EXPECT_FALSE(service.moveEntities(doc, {a, core::Uuid::generate()}, {1, 1}).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(a)->start.x, 0.0);
}

TEST(Entities, TextWidthCountsCharactersNotBytes) {
    drawing::TextEntity latin, cyrillic;
    latin.text = "ABCDE";
    cyrillic.text = "АБВГД";  // те же пять знаков, но десять байт в UTF-8
    EXPECT_DOUBLE_EQ(latin.bounds().width(), cyrillic.bounds().width());
}

// ---------- Преобразования и новые операции правки (2D-1, 2D-2) ----------

TEST(Transform, RotationScalingMirrorMapPointsCorrectly) {
    using geometry::Transform2;
    Point2 p = Transform2::rotation({10, 10}, kPi / 2).apply({20, 10});
    EXPECT_NEAR(p.x, 10.0, 1e-12);
    EXPECT_NEAR(p.y, 20.0, 1e-12);

    p = Transform2::scaling({10, 10}, 3.0).apply({12, 11});
    EXPECT_NEAR(p.x, 16.0, 1e-12);
    EXPECT_NEAR(p.y, 13.0, 1e-12);

    // Отражение относительно прямой y = x меняет координаты местами.
    p = Transform2::mirror({0, 0}, {5, 5}).apply({3, 1});
    EXPECT_NEAR(p.x, 1.0, 1e-12);
    EXPECT_NEAR(p.y, 3.0, 1e-12);
    // Отражение относительно вертикали x = 10.
    p = Transform2::mirror({10, 0}, {10, 7}).apply({4, 2});
    EXPECT_NEAR(p.x, 16.0, 1e-12);
    EXPECT_NEAR(p.y, 2.0, 1e-12);
    // Точка на оси остаётся на месте; двойное отражение возвращает исходную точку.
    auto m = Transform2::mirror({1, 2}, {4, 9});
    Point2 twice = m.apply(m.apply({7.5, -3.25}));
    EXPECT_NEAR(twice.x, 7.5, 1e-9);
    EXPECT_NEAR(twice.y, -3.25, 1e-9);
}

TEST_F(SelectionTest, RectangleIsFourLinesAndOneUndoStep) {
    auto ids = service.addRectangle(doc, {10, 20}, {60, 50});
    ASSERT_TRUE(ids.ok());
    EXPECT_EQ(ids.value().size(), 4u);
    geometry::BBox2 box;
    for (const auto* line : doc.objectsOf<drawing::LineEntity>()) box.expand(line->bounds());
    EXPECT_DOUBLE_EQ(box.min.x, 10.0);
    EXPECT_DOUBLE_EQ(box.max.x, 60.0);
    EXPECT_DOUBLE_EQ(box.min.y, 20.0);
    EXPECT_DOUBLE_EQ(box.max.y, 50.0);
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.size(), 0u);
    EXPECT_FALSE(service.addRectangle(doc, {10, 20}, {10, 50}).ok());  // нулевая ширина
    EXPECT_EQ(doc.size(), 0u);
}

TEST_F(SelectionTest, CopyCreatesNewObjectsAndLeavesOriginals) {
    auto line = service.addLine(doc, {0, 0}, {10, 0}).value();
    auto circle = service.addCircle(doc, {5, 5}, 2).value();
    auto copies = service.copyEntities(doc, {line, circle}, geometry::Transform2::translation({100, 0}));
    ASSERT_TRUE(copies.ok());
    ASSERT_EQ(copies.value().size(), 2u);
    EXPECT_NE(copies.value()[0], line);  // у копии свой постоянный номер
    EXPECT_EQ(doc.size(), 4u);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(line)->start.x, 0.0);  // оригинал на месте
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(copies.value()[0])->start.x, 100.0);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::CircleEntity>(copies.value()[1])->center.x, 105.0);
    ASSERT_TRUE(doc.undo());  // копирование — один шаг отмены
    EXPECT_EQ(doc.size(), 2u);
}

TEST_F(SelectionTest, RotateScaleMirrorKeepShapes) {
    using geometry::Transform2;
    auto circle = service.addCircle(doc, {10, 0}, 4).value();
    auto arc = service.addArc(doc, {0, 0}, 5, 0.0, kPi / 2).value();  // четверть: от 0° до 90°
    auto dim = service.addLinearDimension(doc, {0, 0}, {10, 0}, 8.0).value();

    ASSERT_TRUE(service.transformEntities(doc, {circle, arc, dim}, Transform2::rotation({0, 0}, kPi / 2), "r").ok());
    EXPECT_NEAR(doc.findAs<drawing::CircleEntity>(circle)->center.y, 10.0, 1e-12);
    EXPECT_NEAR(doc.findAs<drawing::CircleEntity>(circle)->radius, 4.0, 1e-12);
    EXPECT_NEAR(doc.findAs<drawing::ArcEntity>(arc)->startAngle, kPi / 2, 1e-12);  // теперь от 90° до 180°
    EXPECT_NEAR(doc.findAs<drawing::ArcEntity>(arc)->endAngle, kPi, 1e-12);
    EXPECT_NEAR(doc.findAs<drawing::LinearDimension>(dim)->value(), 10.0, 1e-12);  // длина не изменилась

    ASSERT_TRUE(service.transformEntities(doc, {circle, dim}, Transform2::scaling({0, 0}, 2.0), "s").ok());
    EXPECT_NEAR(doc.findAs<drawing::CircleEntity>(circle)->radius, 8.0, 1e-12);
    EXPECT_NEAR(doc.findAs<drawing::LinearDimension>(dim)->value(), 20.0, 1e-12);
    EXPECT_NEAR(doc.findAs<drawing::LinearDimension>(dim)->offset, 16.0, 1e-12);

    // Отражение дуги (90°…180°) относительно оси Y даёт дугу 0°…90°, по-прежнему против часовой стрелки.
    ASSERT_TRUE(service.transformEntities(doc, {arc}, Transform2::mirror({0, 0}, {0, 1}), "m").ok());
    const auto* mirrored = doc.findAs<drawing::ArcEntity>(arc);
    EXPECT_NEAR(mirrored->startAngle, 0.0, 1e-9);
    EXPECT_NEAR(mirrored->endAngle, kPi / 2, 1e-9);
    // Середина дуги должна лежать в первой четверти.
    auto mid = geometry::pointOnCircle(mirrored->center, mirrored->radius, (mirrored->startAngle + mirrored->endAngle) / 2);
    EXPECT_GT(mid.x, 0.0);
    EXPECT_GT(mid.y, 0.0);

    // Нулевой масштаб и несуществующий элемент отвергаются, документ не меняется.
    std::string before = doc.toJson().dump();
    EXPECT_FALSE(service.transformEntities(doc, {circle}, Transform2::scaling({0, 0}, 0.0), "bad").ok());
    EXPECT_FALSE(service.transformEntities(doc, {circle, core::Uuid::generate()}, Transform2::translation({1, 1}), "bad").ok());
    EXPECT_EQ(doc.toJson().dump(), before);
}

TEST_F(SelectionTest, MirrorOfDimensionKeepsItOnTheSameSideOfThePart) {
    // Размер под отрезком (отступ вниз). После отражения относительно вертикали он должен остаться снизу.
    auto dim = service.addLinearDimension(doc, {0, 0}, {10, 0}, -8.0).value();
    ASSERT_TRUE(service.transformEntities(doc, {dim}, geometry::Transform2::mirror({20, 0}, {20, 5}), "m").ok());
    const auto* d = doc.findAs<drawing::LinearDimension>(dim);
    EXPECT_LT(d->bounds().min.y, -7.9);
    EXPECT_NEAR(d->bounds().max.y, 0.0, 1e-9);
    EXPECT_NEAR(d->value(), 10.0, 1e-12);
}

TEST_F(SelectionTest, WindowSelectionTakesOnlyFullyEnclosedEntities) {
    auto inside = service.addLine(doc, {10, 10}, {20, 20}).value();
    service.addLine(doc, {15, 15}, {80, 15}).value();  // выходит за рамку
    auto circle = service.addCircle(doc, {30, 30}, 5).value();
    auto ids = graphics::entitiesInside(doc, geometry::BBox2::fromCorners({0, 0}, {40, 40}));
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], inside);
    EXPECT_EQ(ids[1], circle);
}

TEST_F(SelectionTest, PerpendicularAndNearestSnaps) {
    service.addLine(doc, {0, 0}, {100, 0}).value();
    // Из точки (30, 40) перпендикуляр на отрезок приходит в (30, 0).
    auto perpendicular = graphics::findSnap(doc, {31, 0.5}, 2.0, Point2{30, 40});
    ASSERT_TRUE(perpendicular.has_value());
    EXPECT_EQ(perpendicular->kind, graphics::Snap::Kind::Perpendicular);
    EXPECT_NEAR(perpendicular->point.x, 30.0, 1e-12);
    EXPECT_NEAR(perpendicular->point.y, 0.0, 1e-12);

    // Без опорной точки — ближайшая точка на отрезке.
    auto nearest = graphics::findSnap(doc, {70.3, 1.2}, 2.0);
    ASSERT_TRUE(nearest.has_value());
    EXPECT_EQ(nearest->kind, graphics::Snap::Kind::Nearest);
    EXPECT_NEAR(nearest->point.x, 70.3, 1e-12);
    EXPECT_NEAR(nearest->point.y, 0.0, 1e-12);

    // Характерная точка важнее: рядом с концом отрезка выбирается конец.
    auto end = graphics::findSnap(doc, {99.5, 0.3}, 2.0, Point2{30, 40});
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end->kind, graphics::Snap::Kind::Endpoint);

    // Ближайшая точка на окружности.
    service.addCircle(doc, {200, 200}, 10).value();
    auto onCircle = graphics::findSnap(doc, {210.8, 200}, 2.0);
    ASSERT_TRUE(onCircle.has_value());
    EXPECT_EQ(onCircle->kind, graphics::Snap::Kind::Nearest);
    EXPECT_NEAR(onCircle->point.x, 210.0, 1e-9);
}

TEST_F(SelectionTest, LayerStateAndSheetUpdateAreUndoable) {
    auto layer = service.addLayer(doc, "Оси", "dash_dot").value();
    auto line = service.addLine(doc, {0, 0}, {10, 0}).value();
    ASSERT_TRUE(service.setEntitiesLayer(doc, {line}, layer).ok());
    EXPECT_EQ(doc.findAs<drawing::LineEntity>(line)->layerId, layer);
    ASSERT_TRUE(service.setLayerState(doc, layer, false, false).ok());
    EXPECT_FALSE(doc.findAs<drawing::Layer>(layer)->visible);
    ASSERT_TRUE(doc.undo());
    EXPECT_TRUE(doc.findAs<drawing::Layer>(layer)->visible);
    EXPECT_FALSE(service.setEntitiesLayer(doc, {line}, core::Uuid::generate()).ok());

    drawing::SheetSetup setup;
    setup.profileId = "eskd";
    ASSERT_TRUE(service.setupSheet(doc, setup).ok());
    ASSERT_TRUE(service.updateSheet(doc, "A3", true, 1.0, {{"title", "Вал"}}).ok());
    const auto* sheet = doc.objectsOf<drawing::Sheet>().front();
    EXPECT_EQ(sheet->format, "A3");
    EXPECT_TRUE(sheet->landscape);
    EXPECT_EQ(sheet->titleBlock.at("title"), "Вал");
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.objectsOf<drawing::Sheet>().front()->format, "A4");
    EXPECT_FALSE(service.updateSheet(doc, "A3", false, 0.0, {}).ok());  // недопустимая толщина
}

// ---------- Штриховка, обрезка, скругление, фаска, размеры радиуса и угла ----------

namespace {

double strokeArea(const drawing::Hatch& hatch) {
    // Сумма длин линий штриховки, умноженная на шаг, приближает площадь области.
    double total = 0.0;
    for (const auto& [a, b] : hatch.strokes()) total += geometry::distance(a, b);
    return total * hatch.spacing;
}

}  // namespace

TEST(Geometry, LineCircleAndInfiniteLineIntersections) {
    auto params = geometry::segmentCircleParams({0, 0}, {100, 0}, {50, 0}, 10);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_NEAR(params[0], 0.4, 1e-12);
    EXPECT_NEAR(params[1], 0.6, 1e-12);
    EXPECT_TRUE(geometry::segmentCircleParams({0, 20}, {100, 20}, {50, 0}, 10).empty());   // мимо
    EXPECT_EQ(geometry::segmentCircleParams({0, 10}, {100, 10}, {50, 0}, 10).size(), 1u);  // касание
    EXPECT_EQ(geometry::segmentCircleParams({50, 0}, {100, 0}, {50, 0}, 10).size(), 1u);   // начало внутри

    auto x = geometry::lineIntersection({0, 0}, {1, 0}, {5, 5}, {5, 6});  // прямые, а не отрезки
    ASSERT_TRUE(x.has_value());
    EXPECT_NEAR(x->x, 5.0, 1e-12);
    EXPECT_NEAR(x->y, 0.0, 1e-12);
    EXPECT_FALSE(geometry::lineIntersection({0, 0}, {1, 1}, {0, 1}, {1, 2}).has_value());

    auto arc = geometry::tessellateArc({0, 0}, 10, 0.0, kPi / 2, 0.01);
    EXPECT_NEAR(arc.front().x, 10.0, 1e-12);
    EXPECT_NEAR(arc.back().y, 10.0, 1e-9);
    for (std::size_t i = 0; i + 1 < arc.size(); ++i)  // хорда не отходит от дуги дальше допуска
        EXPECT_GT(geometry::length(geometry::midpoint(arc[i], arc[i + 1])), 10.0 - 0.0101);
}

TEST_F(SelectionTest, HatchFillsRectangleAndLeavesHoleEmpty) {
    auto rect = service.addRectangle(doc, {0, 0}, {40, 20}).value();
    auto hatch = service.addHatch(doc, rect, kPi / 4, 1.0);
    ASSERT_TRUE(hatch.ok()) << hatch.error().message;
    const auto* h = doc.findAs<drawing::Hatch>(hatch.value());
    EXPECT_NEAR(strokeArea(*h), 800.0, 800.0 * 0.01);  // площадь 40 × 20
    for (const auto& [a, b] : h->strokes()) {            // все линии под 45° и внутри прямоугольника
        EXPECT_NEAR(std::abs(b.y - a.y), std::abs(b.x - a.x), 1e-9);
        EXPECT_TRUE(a.x > -1e-6 && b.x < 40 + 1e-6 && a.y > -1e-6 && a.y < 20 + 1e-6);
    }

    // С окружностью внутри: отверстие не штрихуется.
    auto circle = service.addCircle(doc, {20, 10}, 5).value();
    auto boundary = rect;
    boundary.push_back(circle);
    auto withHole = service.addHatch(doc, boundary, kPi / 4, 0.5);
    ASSERT_TRUE(withHole.ok());
    const auto* hh = doc.findAs<drawing::Hatch>(withHole.value());
    EXPECT_NEAR(strokeArea(*hh), 800.0 - kPi * 25.0, 8.0);
    for (const auto& [a, b] : hh->strokes())
        EXPECT_GT(geometry::distanceToSegment({20, 10}, a, b), 5.0 - 0.02);  // ни одна линия не заходит в отверстие
}

TEST_F(SelectionTest, HatchFollowsArcsAndRejectsOpenContours) {
    // Полукруг: диаметр по оси X и дуга сверху. Площадь π·r²/2.
    auto line = service.addLine(doc, {-10, 0}, {10, 0}).value();
    auto arc = service.addArc(doc, {0, 0}, 10, 0.0, kPi).value();
    auto hatch = service.addHatch(doc, {line, arc}, kPi / 4, 0.5);
    ASSERT_TRUE(hatch.ok()) << hatch.error().message;
    EXPECT_NEAR(strokeArea(*doc.findAs<drawing::Hatch>(hatch.value())), kPi * 50.0, 2.0);

    std::size_t before = doc.size();
    auto open1 = service.addLine(doc, {100, 0}, {120, 0}).value();
    auto open2 = service.addLine(doc, {120, 0}, {120, 20}).value();
    auto result = service.addHatch(doc, {open1, open2}, kPi / 4, 3.0);  // не замкнуто
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidArgument);
    EXPECT_FALSE(service.addHatch(doc, {}, kPi / 4, 3.0).ok());
    EXPECT_FALSE(service.addHatch(doc, {line, arc}, kPi / 4, 0.0).ok());  // нулевой шаг
    auto text = service.addText(doc, {0, 50}, "т", 3.5).value();
    EXPECT_FALSE(service.addHatch(doc, {text}, kPi / 4, 3.0).ok());  // текст — не контур
    EXPECT_EQ(doc.size(), before + 3);  // ни одной лишней штриховки
}

TEST_F(SelectionTest, HatchIsSelectedByItsLinesNotByArea) {
    auto rect = service.addRectangle(doc, {0, 0}, {40, 20}).value();
    auto hatch = service.addHatch(doc, rect, 0.0, 4.0).value();  // горизонтальные линии y = 2, 6, 10, 14, 18
    EXPECT_EQ(doc.findAs<drawing::Hatch>(hatch)->strokes().size(), 5u);
    EXPECT_EQ(graphics::hitTest(doc, {20, 6.1}, 0.5), hatch);
    EXPECT_FALSE(graphics::hitTest(doc, {20, 8}, 0.5).has_value());  // между линиями — пусто
    EXPECT_EQ(graphics::hitTest(doc, {20, 0.1}, 0.5), rect[0]);       // сторона контура выбирается сама
}

TEST_F(SelectionTest, TrimRemovesPartBetweenNearestIntersections) {
    auto target = service.addLine(doc, {0, 0}, {100, 0}).value();
    service.addLine(doc, {30, -10}, {30, 10}).value();
    service.addLine(doc, {70, -10}, {70, 10}).value();
    auto lines = [&] { return doc.objectsOf<drawing::LineEntity>().size(); };

    // Середина: отрезок распадается на два.
    ASSERT_TRUE(service.trimLine(doc, target, {50, 0.2}).ok());
    EXPECT_EQ(lines(), 4u);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(target)->end.x, 30.0);
    const auto* rest = doc.objectsOf<drawing::LineEntity>().back();
    EXPECT_DOUBLE_EQ(rest->start.x, 70.0);
    EXPECT_DOUBLE_EQ(rest->end.x, 100.0);
    ASSERT_TRUE(doc.undo());  // обрезка — один шаг отмены
    EXPECT_EQ(lines(), 3u);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(target)->end.x, 100.0);

    // Край: отрезок укорачивается.
    ASSERT_TRUE(service.trimLine(doc, target, {10, 0}).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(target)->start.x, 30.0);
    ASSERT_TRUE(service.trimLine(doc, target, {95, 0}).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(target)->end.x, 70.0);
    EXPECT_EQ(lines(), 3u);

    // Теперь отрезок 30…70 упирается концами в вертикали — резать больше нечем.
    auto nothing = service.trimLine(doc, target, {50, 0});
    ASSERT_FALSE(nothing.ok());
    EXPECT_EQ(nothing.error().code, core::ErrorCode::InvalidState);
}

TEST_F(SelectionTest, TrimCutsAtCirclesAndArcsAndKeepsLayer) {
    auto layer = service.addLayer(doc, "Оси", "dash_dot").value();
    auto target = service.addLine(doc, {0, 0}, {100, 0}, layer).value();
    service.addCircle(doc, {50, 0}, 10).value();
    ASSERT_TRUE(service.trimLine(doc, target, {50, 0}).ok());  // убрать часть внутри окружности
    EXPECT_NEAR(doc.findAs<drawing::LineEntity>(target)->end.x, 40.0, 1e-9);
    const auto* rest = doc.objectsOf<drawing::LineEntity>().back();
    EXPECT_NEAR(rest->start.x, 60.0, 1e-9);
    EXPECT_EQ(rest->layerId, layer);  // остаток — на том же слое

    // Дуга пересекает прямую только там, где она есть: верхняя полуокружность не режет ось X внутри себя,
    // но режет в концах дуги.
    document::Document other(document::DocumentKind::Drawing, "t2");
    auto line2 = service.addLine(other, {0, 5}, {100, 5}).value();
    service.addArc(other, {50, 0}, 10, kPi, 2 * kPi - 1e-9).value();  // нижняя полуокружность — выше y = 0 её нет
    EXPECT_FALSE(service.trimLine(other, line2, {50, 5}).ok());
    EXPECT_FALSE(service.trimLine(other, core::Uuid::generate(), {0, 0}).ok());
}

TEST_F(SelectionTest, FilletMakesTangentArcAndShortensLines) {
    auto a = service.addLine(doc, {0, 0}, {50, 0}).value();
    auto b = service.addLine(doc, {0, 0}, {0, 30}).value();
    ASSERT_TRUE(service.filletLines(doc, a, {40, 0}, b, {0, 20}, 10.0).ok());

    const auto* la = doc.findAs<drawing::LineEntity>(a);
    const auto* lb = doc.findAs<drawing::LineEntity>(b);
    EXPECT_NEAR(la->start.x, 10.0, 1e-9);
    EXPECT_NEAR(la->end.x, 50.0, 1e-9);
    EXPECT_NEAR(lb->start.y, 10.0, 1e-9);
    ASSERT_EQ(doc.objectsOf<drawing::ArcEntity>().size(), 1u);
    const auto* arc = doc.objectsOf<drawing::ArcEntity>().front();
    EXPECT_NEAR(arc->center.x, 10.0, 1e-9);
    EXPECT_NEAR(arc->center.y, 10.0, 1e-9);
    EXPECT_NEAR(arc->radius, 10.0, 1e-12);
    // Дуга — четверть окружности от 180° до 270°, концы точно в точках сопряжения.
    EXPECT_NEAR(geometry::normalizeAngle(arc->endAngle - arc->startAngle), kPi / 2, 1e-9);
    auto start = geometry::pointOnCircle(arc->center, arc->radius, arc->startAngle);
    auto end = geometry::pointOnCircle(arc->center, arc->radius, arc->endAngle);
    EXPECT_TRUE((geometry::almostEqual(start, {0, 10}, 1e-9) && geometry::almostEqual(end, {10, 0}, 1e-9)));

    ASSERT_TRUE(doc.undo());  // один шаг отмены возвращает оба отрезка и убирает дугу
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::LineEntity>(a)->start.x, 0.0);
    EXPECT_TRUE(doc.objectsOf<drawing::ArcEntity>().empty());
}

TEST_F(SelectionTest, FilletOnCrossingLinesKeepsPickedSidesAndZeroRadiusMakesSharpCorner) {
    // Крест: какая четверть останется, решают указанные точки.
    auto a = service.addLine(doc, {-50, 0}, {50, 0}).value();
    auto b = service.addLine(doc, {0, -50}, {0, 50}).value();
    ASSERT_TRUE(service.filletLines(doc, a, {-30, 0}, b, {0, -30}, 5.0).ok());  // левая и нижняя стороны
    const auto* la = doc.findAs<drawing::LineEntity>(a);
    const auto* lb = doc.findAs<drawing::LineEntity>(b);
    EXPECT_NEAR(std::min(la->start.x, la->end.x), -50.0, 1e-9);
    EXPECT_NEAR(std::max(la->start.x, la->end.x), -5.0, 1e-9);
    EXPECT_NEAR(std::max(lb->start.y, lb->end.y), -5.0, 1e-9);
    const auto* arc = doc.objectsOf<drawing::ArcEntity>().front();
    EXPECT_NEAR(arc->center.x, -5.0, 1e-9);
    EXPECT_NEAR(arc->center.y, -5.0, 1e-9);

    // Радиус 0: два не доходящих друг до друга отрезка доводятся до острого угла.
    document::Document other(document::DocumentKind::Drawing, "t2");
    auto c = service.addLine(other, {10, 0}, {50, 0}).value();
    auto d = service.addLine(other, {0, 10}, {0, 40}).value();
    ASSERT_TRUE(service.filletLines(other, c, {30, 0}, d, {0, 30}, 0.0).ok());
    EXPECT_NEAR(other.findAs<drawing::LineEntity>(c)->start.x, 0.0, 1e-9);
    EXPECT_NEAR(other.findAs<drawing::LineEntity>(d)->start.y, 0.0, 1e-9);
    EXPECT_TRUE(other.objectsOf<drawing::ArcEntity>().empty());
}

TEST_F(SelectionTest, FilletAndChamferRejectImpossibleCases) {
    auto a = service.addLine(doc, {0, 0}, {50, 0}).value();
    auto b = service.addLine(doc, {0, 0}, {0, 30}).value();
    auto parallel = service.addLine(doc, {0, 10}, {50, 10}).value();
    std::string before = doc.toJson().dump();
    EXPECT_FALSE(service.filletLines(doc, a, {40, 0}, b, {0, 20}, 40.0).ok());        // не помещается
    EXPECT_FALSE(service.filletLines(doc, a, {40, 0}, parallel, {40, 10}, 5.0).ok());  // параллельны
    EXPECT_FALSE(service.filletLines(doc, a, {40, 0}, a, {40, 0}, 5.0).ok());          // один и тот же
    EXPECT_FALSE(service.filletLines(doc, a, {40, 0}, b, {0, 20}, -1.0).ok());         // отрицательный радиус
    EXPECT_FALSE(service.chamferLines(doc, a, {40, 0}, b, {0, 20}, 35.0).ok());
    EXPECT_EQ(doc.toJson().dump(), before);  // документ не тронут

    ASSERT_TRUE(service.chamferLines(doc, a, {40, 0}, b, {0, 20}, 8.0).ok());
    EXPECT_NEAR(doc.findAs<drawing::LineEntity>(a)->start.x, 8.0, 1e-9);
    EXPECT_NEAR(doc.findAs<drawing::LineEntity>(b)->start.y, 8.0, 1e-9);
    const auto* bridge = doc.objectsOf<drawing::LineEntity>().back();
    EXPECT_NEAR(geometry::distance(bridge->start, bridge->end), 8.0 * std::sqrt(2.0), 1e-9);
}

TEST_F(SelectionTest, RadialAndAngularDimensionsShowCorrectValues) {
    auto radius = service.addRadialDimension(doc, {50, 50}, 15, kPi / 4, false).value();
    auto diameter = service.addRadialDimension(doc, {50, 50}, 15, kPi / 2, true).value();
    // Угол от оси X до луча под 60° против часовой стрелки.
    auto angle = service.addAngularDimension(doc, {0, 0}, {10, 0}, {5, 5 * std::sqrt(3.0)}, 25).value();
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::RadialDimension>(radius)->value(), 15.0);
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::RadialDimension>(diameter)->value(), 30.0);
    EXPECT_NEAR(doc.findAs<drawing::AngularDimension>(angle)->valueDegrees(), 60.0, 1e-9);

    auto scene = graphics::buildScene(doc, nullptr);
    auto hasText = [&](const std::string& text) {
        for (const auto& item : scene.items)
            if (item.kind == graphics::SceneItem::Kind::Text && item.text == text) return true;
        return false;
    };
    EXPECT_TRUE(hasText("R15"));
    EXPECT_TRUE(hasText("Ø30"));
    EXPECT_TRUE(hasText("60°"));

    // Обратный порядок сторон измеряет дополнение до 360°.
    auto reflex = service.addAngularDimension(doc, {0, 0}, {5, 5 * std::sqrt(3.0)}, {10, 0}, 25).value();
    EXPECT_NEAR(doc.findAs<drawing::AngularDimension>(reflex)->valueDegrees(), 300.0, 1e-9);

    // Отражение не меняет измеряемый угол и радиус.
    ASSERT_TRUE(service.transformEntities(doc, {angle, radius}, geometry::Transform2::mirror({0, 0}, {0, 1}), "m").ok());
    EXPECT_NEAR(doc.findAs<drawing::AngularDimension>(angle)->valueDegrees(), 60.0, 1e-9);
    EXPECT_NEAR(doc.findAs<drawing::RadialDimension>(radius)->center.x, -50.0, 1e-9);
    EXPECT_NEAR(doc.findAs<drawing::RadialDimension>(radius)->angle, 3 * kPi / 4, 1e-9);

    EXPECT_FALSE(service.addRadialDimension(doc, {0, 0}, 0.0, 0.0, false).ok());
    EXPECT_FALSE(service.addAngularDimension(doc, {0, 0}, {0, 0}, {1, 1}, 10).ok());
    EXPECT_FALSE(service.addAngularDimension(doc, {0, 0}, {5, 0}, {9, 0}, 10).ok());  // стороны совпадают
}

TEST_F(SelectionTest, NewEntitiesSurviveSaveFormatRoundTrip) {
    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    auto rect = service.addRectangle(doc, {0, 0}, {40, 20}).value();
    service.addHatch(doc, rect, kPi / 3, 2.5).value();
    service.addRadialDimension(doc, {50, 50}, 15, 1.0, true).value();
    service.addAngularDimension(doc, {0, 0}, {10, 0}, {0, 10}, 25).value();
    auto restored = document::Document::fromJson(doc.toJson(), factory);
    ASSERT_TRUE(restored.ok()) << restored.error().message;
    EXPECT_EQ(restored.value()->toJson().dump(), doc.toJson().dump());
    EXPECT_EQ(restored.value()->objectsOf<drawing::Hatch>().front()->strokes().size(),
              doc.objectsOf<drawing::Hatch>().front()->strokes().size());
}

// ---------- Правка свойств, допуски, выноски ----------

TEST_F(SelectionTest, EditEntityChangesOnlyApplicablePropertiesAndIsUndoable) {
    auto text = service.addText(doc, {10, 10}, "Старый текст", 3.5).value();
    auto circle = service.addCircle(doc, {50, 50}, 10).value();
    auto rect = service.addRectangle(doc, {0, 0}, {40, 20}).value();
    auto hatch = service.addHatch(doc, rect, kPi / 4, 3.0).value();

    drawing::EntityEdit edit;
    edit.text = "Новый текст";
    edit.textHeight = 5.0;
    ASSERT_TRUE(service.editEntity(doc, text, edit).ok());
    EXPECT_EQ(doc.findAs<drawing::TextEntity>(text)->text, "Новый текст");
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::TextEntity>(text)->height, 5.0);
    ASSERT_TRUE(doc.undo());  // обе правки — один шаг отмены
    EXPECT_EQ(doc.findAs<drawing::TextEntity>(text)->text, "Старый текст");
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::TextEntity>(text)->height, 3.5);

    drawing::EntityEdit radius;
    radius.radius = 25.0;
    ASSERT_TRUE(service.editEntity(doc, circle, radius).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::CircleEntity>(circle)->radius, 25.0);

    drawing::EntityEdit hatchEdit;
    hatchEdit.hatchSpacing = 2.0;
    hatchEdit.hatchAngleDegrees = 60.0;
    ASSERT_TRUE(service.editEntity(doc, hatch, hatchEdit).ok());
    EXPECT_DOUBLE_EQ(doc.findAs<drawing::Hatch>(hatch)->spacing, 2.0);
    EXPECT_NEAR(doc.findAs<drawing::Hatch>(hatch)->angle, kPi / 3, 1e-12);

    // Свойство, которого у элемента нет, и недопустимые значения — ошибка, документ не меняется.
    std::string before = doc.toJson().dump();
    EXPECT_FALSE(service.editEntity(doc, text, radius).ok());        // радиус у текста
    EXPECT_FALSE(service.editEntity(doc, circle, edit).ok());        // текст у окружности
    drawing::EntityEdit bad;
    bad.radius = -1.0;
    EXPECT_FALSE(service.editEntity(doc, circle, bad).ok());
    drawing::EntityEdit empty;
    empty.text = "";
    EXPECT_FALSE(service.editEntity(doc, text, empty).ok());
    // Частично верная правка не применяется наполовину.
    drawing::EntityEdit mixed;
    mixed.text = "Хороший текст";
    mixed.textHeight = -3.0;
    EXPECT_FALSE(service.editEntity(doc, text, mixed).ok());
    EXPECT_FALSE(service.editEntity(doc, core::Uuid::generate(), edit).ok());
    EXPECT_EQ(doc.toJson().dump(), before);
}

TEST_F(SelectionTest, DimensionToleranceIsShownAndSaved) {
    auto dim = service.addLinearDimension(doc, {0, 0}, {80, 0}, -12).value();
    drawing::EntityEdit edit;
    edit.note = drawing::DimensionNote{"", "+0,2", "−0,1"};
    ASSERT_TRUE(service.editEntity(doc, dim, edit).ok());

    auto texts = [&] {
        std::vector<graphics::SceneItem> out;
        for (const auto& item : graphics::buildScene(doc, nullptr).items)
            if (item.kind == graphics::SceneItem::Kind::Text) out.push_back(item);
        return out;
    };
    auto items = texts();
    ASSERT_EQ(items.size(), 3u);  // число и два отклонения
    EXPECT_EQ(items[0].text, "80");
    EXPECT_EQ(items[1].text, "+0,2");
    EXPECT_EQ(items[2].text, "−0,1");
    EXPECT_LT(items[1].textHeight, items[0].textHeight);  // отклонения мельче числа
    EXPECT_GT(items[1].a.x, items[0].a.x);                 // и стоят справа от него
    EXPECT_GT(items[1].a.y, items[2].a.y);                 // верхнее над нижним

    // Обозначение посадки пишется вплотную к числу.
    edit.note = drawing::DimensionNote{"H7", "", ""};
    ASSERT_TRUE(service.editEntity(doc, dim, edit).ok());
    items = texts();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].text, "80H7");

    // У диаметра — тоже; у углового размера допусков нет.
    auto diameter = service.addRadialDimension(doc, {50, 50}, 15, 0.5, true).value();
    ASSERT_TRUE(service.editEntity(doc, diameter, edit).ok());
    auto angle = service.addAngularDimension(doc, {0, 0}, {10, 0}, {0, 10}, 20).value();
    EXPECT_FALSE(service.editEntity(doc, angle, edit).ok());

    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    auto restored = document::Document::fromJson(doc.toJson(), factory);
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.value()->findAs<drawing::LinearDimension>(dim)->note.suffix, "H7");
    EXPECT_EQ(restored.value()->toJson().dump(), doc.toJson().dump());
}

TEST_F(SelectionTest, LeaderHasArrowShelfAndText) {
    auto id = service.addLeader(doc, {10, 10}, {30, 40}, "Фаска", 3.5);
    ASSERT_TRUE(id.ok());
    const auto* leader = doc.findAs<drawing::Leader>(id.value());
    EXPECT_GT(leader->shelfEnd().x, 30.0);       // стрелка слева — полка уходит вправо
    EXPECT_DOUBLE_EQ(leader->shelfEnd().y, 40.0);  // полка горизонтальна

    auto scene = graphics::buildScene(doc, nullptr);
    int lines = 0, texts = 0;
    for (const auto& item : scene.items) {
        lines += item.kind == graphics::SceneItem::Kind::Line;
        texts += item.kind == graphics::SceneItem::Kind::Text;
    }
    EXPECT_EQ(lines, 4);  // линия, полка и два штриха стрелки
    EXPECT_EQ(texts, 1);
    EXPECT_EQ(graphics::hitTest(doc, {20, 25}, 0.5), id.value());  // выбирается щелчком по линии

    // Стрелка справа — полка уходит влево; отражение переворачивает выноску.
    auto left = service.addLeader(doc, {100, 10}, {80, 40}, "Отверстие", 3.5).value();
    EXPECT_LT(doc.findAs<drawing::Leader>(left)->shelfEnd().x, 80.0);
    ASSERT_TRUE(service.transformEntities(doc, {id.value()}, geometry::Transform2::mirror({0, 0}, {0, 1}), "m").ok());
    EXPECT_LT(doc.findAs<drawing::Leader>(id.value())->shelfEnd().x, -30.0);

    drawing::EntityEdit edit;
    edit.text = "Фаска 2×45°";
    ASSERT_TRUE(service.editEntity(doc, id.value(), edit).ok());
    EXPECT_FALSE(service.addLeader(doc, {0, 0}, {0, 0}, "т", 3.5).ok());
    EXPECT_FALSE(service.addLeader(doc, {0, 0}, {5, 5}, "", 3.5).ok());
}

// Знак шероховатости (ГОСТ 2.309-73, требование 2D-9).
TEST_F(SelectionTest, RoughnessSignFollowsStandardProportions) {
    using Kind = drawing::RoughnessSign::Kind;
    // Знак со значением: две черты, полка и надпись под полкой.
    auto id = service.addRoughness(doc, {{50, 50}, Kind::Any, "Ra 3,2", "", 0, 3.5, {}});
    ASSERT_TRUE(id.ok()) << id.error().message;
    const auto* sign = doc.findAs<drawing::RoughnessSign>(id.value());
    auto parts = sign->primitives();
    ASSERT_EQ(parts.size(), 4u);
    // Короткая черта высотой h = 3,5 (высота цифр), длинная — в пределах (1,5…5)h; наклон черт 60°.
    EXPECT_NEAR(parts[0].b.y - 50.0, 3.5, 1e-9);
    const double bigH = parts[1].b.y - 50.0;
    EXPECT_GE(bigH, 1.5 * 3.5);
    EXPECT_LE(bigH, 5.0 * 3.5);
    EXPECT_NEAR(std::atan2(parts[1].b.y - 50.0, parts[1].b.x - 50.0), std::numbers::pi / 3.0, 1e-9);
    EXPECT_NEAR(std::atan2(parts[0].b.y - 50.0, 50.0 - parts[0].b.x), std::numbers::pi / 3.0, 1e-9);
    // Полка горизонтальна и начинается от конца длинной черты; надпись — под полкой.
    EXPECT_DOUBLE_EQ(parts[2].a.y, parts[2].b.y);
    EXPECT_NEAR(parts[2].a.x, parts[1].b.x, 1e-9);
    EXPECT_EQ(parts[3].text, "Ra 3,2");
    EXPECT_LT(parts[3].a.y + 3.5, parts[2].a.y);
    EXPECT_EQ(sign->lineTypeId, std::optional<std::string>("solid_thin"));  // тонкая линия — около половины основной
    EXPECT_EQ(graphics::hitTest(doc, {50.5, 50.9}, 0.5), id.value());

    // Знак без надписей — без полки; его можно повернуть к вертикальной линии.
    auto bare = service.addRoughness(doc, {{80, 50}, Kind::Removal, "", "", 1, 3.5, {}});
    ASSERT_TRUE(bare.ok());
    parts = doc.findAs<drawing::RoughnessSign>(bare.value())->primitives();
    ASSERT_EQ(parts.size(), 3u);                     // две черты и перемычка «удаление материала»
    EXPECT_NEAR(parts[0].b.x - 80.0, -3.5, 1e-9);    // повёрнут на четверть: «высота» ушла влево
    auto circle = service.addRoughness(doc, {{100, 50}, Kind::NoRemoval, "", "", 0, 3.5, {}});
    parts = doc.findAs<drawing::RoughnessSign>(circle.value())->primitives();
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[2].kind, drawing::Primitive::Kind::Circle);

    // Значение без символа параметра и повёрнутый знак с надписью отклоняются.
    EXPECT_FALSE(service.addRoughness(doc, {{0, 0}, Kind::Any, "3,2", "", 0, 3.5, {}}).ok());
    EXPECT_FALSE(service.addRoughness(doc, {{0, 0}, Kind::Any, "Ra", "", 0, 3.5, {}}).ok());
    EXPECT_FALSE(service.addRoughness(doc, {{0, 0}, Kind::Any, "Ra 3,2", "", 1, 3.5, {}}).ok());
    EXPECT_TRUE(service.addRoughness(doc, {{0, 0}, Kind::Any, "Rz 50", "", 0, 3.5, {}}).ok());
    EXPECT_TRUE(service.addRoughness(doc, {{0, 0}, Kind::Any, "t50 70", "", 0, 3.5, {}}).ok());

    // Перенос двигает знак; отмена убирает его.
    ASSERT_TRUE(service.transformEntities(doc, {id.value()}, geometry::Transform2::translation({10, 5}), "m").ok());
    EXPECT_NEAR(doc.findAs<drawing::RoughnessSign>(id.value())->position.x, 60.0, 1e-9);
}

// Блоки (требование 2D-7): рисунок собирается один раз и вставляется много раз.
TEST_F(SelectionTest, BlocksAreInsertedRedefinedAndExploded) {
    // Значок: квадрат 10×10 с окружностью и подписью.
    std::vector<core::Uuid> ids;
    for (const auto& [a, b] : std::vector<std::pair<geometry::Point2, geometry::Point2>>{
             {{200, 200}, {210, 200}}, {{210, 200}, {210, 210}}, {{210, 210}, {200, 210}}, {{200, 210}, {200, 200}}})
        ids.push_back(service.addLine(doc, a, b).value());
    ids.push_back(service.addCircle(doc, {205, 205}, 3).value());
    ids.push_back(service.addText(doc, {212, 203}, "А1", 3.5).value());
    const std::size_t before = doc.objectsOf<drawing::Entity>().size();

    auto block = service.createBlock(doc, "Значок", ids, {200, 200}, true);
    ASSERT_TRUE(block.ok()) << block.error().message;
    // Шесть исходных элементов заменены одной вставкой на том же месте.
    EXPECT_EQ(doc.objectsOf<drawing::Entity>().size(), before - 5);
    ASSERT_EQ(doc.objectsOf<drawing::BlockReference>().size(), 1u);
    const auto* first = doc.objectsOf<drawing::BlockReference>().front();
    EXPECT_NEAR(first->bounds().min.x, 200.0, 1e-9);
    EXPECT_EQ(graphics::hitTest(doc, {205, 200}, 0.5), first->id());  // выбирается щелчком по своей линии

    // Вторая вставка — повёрнутая на 90° и вдвое крупнее: квадрат уходит влево-вверх от базовой точки.
    auto second = service.insertBlock(doc, "Значок", {300, 100}, 90.0, 2.0);
    ASSERT_TRUE(second.ok()) << second.error().message;
    const auto box = doc.findAs<drawing::BlockReference>(second.value())->bounds();
    EXPECT_NEAR(box.min.x, 280.0, 1e-6);
    EXPECT_GT(box.max.y, 120.0);  // квадрат 20×20 и подпись над ним
    EXPECT_NEAR(box.min.y, 100.0, 1e-6);
    EXPECT_EQ(service.blockNames(doc), std::vector<std::string>{"Значок"});

    // Переопределение блока меняет все вставки сразу.
    auto cross1 = service.addLine(doc, {0, 0}, {10, 10}).value();
    auto cross2 = service.addLine(doc, {0, 10}, {10, 0}).value();
    ASSERT_TRUE(service.createBlock(doc, "Значок", {cross1, cross2}, {0, 0}, false).ok());
    EXPECT_EQ(doc.objectsOf<drawing::BlockDefinition>().size(), 1u);
    for (const auto* reference : doc.objectsOf<drawing::BlockReference>()) EXPECT_EQ(reference->primitives().size(), 2u);
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.findAs<drawing::BlockReference>(second.value())->primitives().size(), 6u);

    // Разбивка превращает вставку в обычные элементы на тех же местах.
    auto parts = service.explodeBlock(doc, second.value());
    ASSERT_TRUE(parts.ok());
    EXPECT_EQ(parts.value().size(), 6u);
    EXPECT_EQ(doc.findAs<drawing::BlockReference>(second.value()), nullptr);
    const auto* circle = [&]() -> const drawing::CircleEntity* {
        for (const auto& id : parts.value())
            if (const auto* c = doc.findAs<drawing::CircleEntity>(id)) return c;
        return nullptr;
    }();
    ASSERT_NE(circle, nullptr);
    EXPECT_NEAR(circle->radius, 6.0, 1e-9);
    EXPECT_NEAR(circle->center.x, 290.0, 1e-9);  // (5; 5) повёрнуто на 90° и увеличено вдвое: (−10; 10)
    EXPECT_NEAR(circle->center.y, 110.0, 1e-9);

    // Отказы: неизвестный блок, пустое имя, пустой набор, размер в блоке.
    EXPECT_FALSE(service.insertBlock(doc, "Нет такого", {0, 0}).ok());
    EXPECT_FALSE(service.createBlock(doc, "", {cross1}, {0, 0}, false).ok());
    EXPECT_FALSE(service.createBlock(doc, "Пусто", {}, {0, 0}, false).ok());
    auto dimension = service.addLinearDimension(doc, {0, 0}, {10, 0}, 5.0);
    ASSERT_TRUE(dimension.ok());
    EXPECT_FALSE(service.createBlock(doc, "С размером", {dimension.value()}, {0, 0}, false).ok());
    EXPECT_FALSE(service.insertBlock(doc, "Значок", {0, 0}, 0.0, 0.0).ok());
}
