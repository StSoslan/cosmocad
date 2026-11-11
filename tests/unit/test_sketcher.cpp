// Ограничения эскиза и решатель (требования ЭС-1, ЭС-3, ЭС-4, ЭС-5).
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numbers>

#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/IDrawingService.h"
#include "cosmocad/sketcher/Sketcher.h"

using namespace cosmocad;
using geometry::Point2;
using sketcher::ConstraintKind;
using sketcher::PointRole;
using sketcher::SketchState;

namespace {

class Sketch : public ::testing::Test {
protected:
    document::Document doc{document::DocumentKind::Drawing, "эскиз"};
    drawing::DrawingService impl;
    drawing::IDrawingService& draw = impl;
    sketcher::SketchService sketch;

    core::Uuid line(Point2 a, Point2 b) { return draw.addLine(doc, a, b).value(); }
    const drawing::LineEntity& L(const core::Uuid& id) { return *doc.findAs<drawing::LineEntity>(id); }
    const drawing::CircleEntity& C(const core::Uuid& id) { return *doc.findAs<drawing::CircleEntity>(id); }
    double length(const core::Uuid& id) { return geometry::distance(L(id).start, L(id).end); }
    core::Result<core::Uuid> add(ConstraintKind kind, std::vector<sketcher::Ref> refs, double value = 0.0) {
        return sketch.addConstraint(doc, {kind, std::move(refs), value});
    }
    // Четыре отрезка, нарисованные «от руки» почти прямоугольником, с совпадающими углами.
    std::vector<core::Uuid> roughRectangle() {
        std::vector<core::Uuid> s = {line({0, 0}, {52, 3}), line({52, 3}, {49, 31}), line({49, 31}, {-2, 28}), line({-2, 28}, {0, 0})};
        for (std::size_t i = 0; i < 4; ++i)
            EXPECT_TRUE(add(ConstraintKind::Coincident, {{s[i], PointRole::End}, {s[(i + 1) % 4], PointRole::Start}}).ok());
        return s;
    }
};

}  // namespace

TEST_F(Sketch, RoughQuadBecomesExactRectangle) {
    const auto s = roughRectangle();
    ASSERT_TRUE(add(ConstraintKind::Horizontal, {{s[0]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Vertical, {{s[1]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Parallel, {{s[2]}, {s[0]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Perpendicular, {{s[3]}, {s[0]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Length, {{s[0]}}, 50.0).ok());
    ASSERT_TRUE(add(ConstraintKind::Length, {{s[1]}}, 30.0).ok());

    EXPECT_NEAR(L(s[0]).start.y, L(s[0]).end.y, 1e-7);
    EXPECT_NEAR(L(s[1]).start.x, L(s[1]).end.x, 1e-7);
    EXPECT_NEAR(length(s[0]), 50.0, 1e-7);
    EXPECT_NEAR(length(s[2]), 50.0, 1e-6);
    EXPECT_NEAR(length(s[1]), 30.0, 1e-7);
    EXPECT_NEAR(length(s[3]), 30.0, 1e-6);
    for (std::size_t i = 0; i < 4; ++i) EXPECT_LT(geometry::distance(L(s[i]).end, L(s[(i + 1) % 4]).start), 1e-7);
    // Фигура осталась рядом с тем местом, где была нарисована, и не перевернулась (ЭС-4).
    EXPECT_NEAR(L(s[0]).start.x, 0.0, 3.0);
    EXPECT_NEAR(L(s[0]).start.y, 0.0, 3.0);
    EXPECT_GT(L(s[1]).end.y, L(s[1]).start.y);

    // Размеры и форма заданы, но прямоугольник можно двигать: две степени свободы (ЭС-3).
    auto report = sketch.analyze(doc);
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().solved);
    EXPECT_EQ(report.value().freedom, 2);
    EXPECT_EQ(report.value().state(), SketchState::UnderDefined);
    // Закрепили одну сторону — эскиз определён полностью.
    ASSERT_TRUE(add(ConstraintKind::Fixed, {{s[0]}}).ok());
    report = sketch.analyze(doc);
    EXPECT_EQ(report.value().freedom, 0);
    EXPECT_EQ(report.value().state(), SketchState::FullyDefined);
}

TEST_F(Sketch, DimensionChangeMovesGeometryAndConflictIsRefused) {
    const auto s = roughRectangle();
    ASSERT_TRUE(add(ConstraintKind::Horizontal, {{s[0]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Horizontal, {{s[2]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Vertical, {{s[1]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Vertical, {{s[3]}}).ok());
    auto width = add(ConstraintKind::Length, {{s[0]}}, 50.0);
    ASSERT_TRUE(width.ok());

    // Меняем размер — фигура перестраивается, оставаясь прямоугольником.
    ASSERT_TRUE(sketch.setValue(doc, width.value(), 80.0).ok());
    EXPECT_NEAR(length(s[0]), 80.0, 1e-7);
    EXPECT_NEAR(length(s[2]), 80.0, 1e-6);
    EXPECT_NEAR(L(s[1]).start.x, L(s[1]).end.x, 1e-7);
    ASSERT_TRUE(doc.undo());
    EXPECT_NEAR(length(s[0]), 50.0, 1e-7);

    // Противоречие: противоположная сторона не может быть другой длины. Отказ, ничего не меняется.
    const std::size_t before = doc.objectsOf<sketcher::Constraint>().size();
    auto conflict = add(ConstraintKind::Length, {{s[2]}}, 60.0);
    ASSERT_FALSE(conflict.ok());
    EXPECT_NE(conflict.error().message.find("Длина 1"), std::string::npos) << conflict.error().message;
    EXPECT_EQ(doc.objectsOf<sketcher::Constraint>().size(), before);
    EXPECT_NEAR(length(s[2]), 50.0, 1e-6);
    // Лишнее, но не противоречащее ограничение принимается; эскиз считается переопределённым.
    ASSERT_TRUE(add(ConstraintKind::Length, {{s[2]}}, 50.0).ok());
    auto report = sketch.analyze(doc);
    EXPECT_TRUE(report.value().solved);
    EXPECT_GT(report.value().redundant, 0);
    EXPECT_EQ(report.value().state(), SketchState::OverDefined);

    // Недопустимые значения и неподходящие элементы.
    EXPECT_FALSE(sketch.setValue(doc, width.value(), -5.0).ok());
    EXPECT_FALSE(add(ConstraintKind::Length, {{s[0]}}, 0.0).ok());
    EXPECT_FALSE(add(ConstraintKind::Parallel, {{s[0]}}).ok());                 // нужен второй отрезок
    EXPECT_FALSE(add(ConstraintKind::Parallel, {{s[0]}, {s[0]}}).ok());         // один и тот же
    EXPECT_FALSE(add(ConstraintKind::Radius, {{s[0]}}, 5.0).ok());              // радиус — не у отрезка
    EXPECT_FALSE(add(ConstraintKind::Horizontal, {{core::Uuid::generate()}}).ok());
}

TEST_F(Sketch, CirclesTangencyAnglesAndSymmetry) {
    const auto base = line({0, 0}, {100, 0});
    ASSERT_TRUE(add(ConstraintKind::Fixed, {{base}}).ok());
    const auto circle = draw.addCircle(doc, {40, 23}, 18).value();
    // Окружность радиуса 20 касается закреплённого отрезка: центр встаёт на высоте 20 с той же стороны.
    ASSERT_TRUE(add(ConstraintKind::Radius, {{circle}}, 20.0).ok());
    ASSERT_TRUE(add(ConstraintKind::Tangent, {{circle}, {base}}).ok());
    EXPECT_NEAR(C(circle).radius, 20.0, 1e-8);
    EXPECT_NEAR(C(circle).center.y, 20.0, 1e-7);
    EXPECT_NEAR(C(circle).center.x, 40.0, 1e-6);  // вдоль отрезка окружность не сдвинулась

    // Вторая окружность: равный радиус, касается первой снаружи и тоже лежит на отрезке.
    const auto second = draw.addCircle(doc, {85, 15}, 12).value();
    ASSERT_TRUE(add(ConstraintKind::EqualRadius, {{second}, {circle}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Tangent, {{second}, {base}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Tangent, {{second}, {circle}}).ok());
    EXPECT_NEAR(C(second).radius, 20.0, 1e-7);
    EXPECT_NEAR(geometry::distance(C(second).center, C(circle).center), 40.0, 1e-6);
    EXPECT_NEAR(C(second).center.y, 20.0, 1e-6);
    // Третья — концентрична первой.
    const auto inner = draw.addCircle(doc, {43, 25}, 8).value();
    ASSERT_TRUE(add(ConstraintKind::Concentric, {{inner}, {circle}}).ok());
    EXPECT_LT(geometry::distance(C(inner).center, C(circle).center), 1e-7);
    EXPECT_NEAR(C(inner).radius, 8.0, 1e-9);

    // Отрезок под углом 30° к закреплённому, заданной длины; его начало — на прямой основания.
    const auto ray = line({10, 1}, {60, 35});
    ASSERT_TRUE(add(ConstraintKind::PointOnLine, {{ray, PointRole::Start}, {base}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Angle, {{base}, {ray}}, 30.0).ok());
    ASSERT_TRUE(add(ConstraintKind::Length, {{ray}}, 40.0).ok());
    EXPECT_NEAR(L(ray).start.y, 0.0, 1e-7);
    EXPECT_NEAR(length(ray), 40.0, 1e-7);
    EXPECT_NEAR(std::atan2(L(ray).end.y - L(ray).start.y, L(ray).end.x - L(ray).start.x), std::numbers::pi / 6.0, 1e-8);

    // Симметрия: концы двух отрезков зеркальны относительно вертикальной оси x = 200.
    const auto axis = line({200, -10}, {200, 60});
    ASSERT_TRUE(add(ConstraintKind::Fixed, {{axis}}).ok());
    const auto left = line({170, 10}, {180, 40}), right = line({232, 12}, {221, 37});
    ASSERT_TRUE(add(ConstraintKind::Symmetric, {{left, PointRole::Start}, {right, PointRole::Start}, {axis}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Symmetric, {{left, PointRole::End}, {right, PointRole::End}, {axis}}).ok());
    EXPECT_NEAR(L(left).start.x + L(right).start.x, 400.0, 1e-7);
    EXPECT_NEAR(L(left).start.y, L(right).start.y, 1e-7);
    EXPECT_NEAR(L(left).end.x + L(right).end.x, 400.0, 1e-7);
    // Расстояние между двумя точками.
    ASSERT_TRUE(add(ConstraintKind::Distance, {{left, PointRole::Start}, {right, PointRole::Start}}, 70.0).ok());
    EXPECT_NEAR(L(right).start.x - L(left).start.x, 70.0, 1e-7);
}

TEST_F(Sketch, SolveAfterManualEditAndOrphans) {
    const auto s = roughRectangle();
    ASSERT_TRUE(add(ConstraintKind::Horizontal, {{s[0]}}).ok());
    ASSERT_TRUE(add(ConstraintKind::Vertical, {{s[1]}}).ok());
    // Пользователь сдвинул одну сторону обычным инструментом — ограничения нарушены.
    ASSERT_TRUE(draw.transformEntities(doc, {s[1]}, geometry::Transform2::translation({15, 4}), "сдвиг").ok());
    EXPECT_FALSE(sketch.analyze(doc).value().satisfied);  // сейчас нарушены
    EXPECT_TRUE(sketch.analyze(doc).value().solved);       // но выполнимы
    auto solved = sketch.solve(doc);
    ASSERT_TRUE(solved.ok());
    EXPECT_TRUE(solved.value().solved);
    EXPECT_GT(solved.value().movedEntities, 0);
    EXPECT_LT(geometry::distance(L(s[0]).end, L(s[1]).start), 1e-7);
    EXPECT_TRUE(sketch.analyze(doc).value().satisfied);
    // Повторный пересчёт ничего не меняет и не оставляет пустого шага отмены.
    const std::string undoName = doc.undoName();
    EXPECT_EQ(sketch.solve(doc).value().movedEntities, 0);
    EXPECT_EQ(doc.undoName(), undoName);

    // Сохранение и чтение ограничения; удаление элемента оставляет «сироту», которую убирает removeOrphans.
    EXPECT_EQ(sketch.constraintsOf(doc, s[0]).size(), 3u);  // два совпадения и горизонтальность
    ASSERT_TRUE(draw.removeObjects(doc, {s[2]}).ok());
    EXPECT_FALSE(sketch.solve(doc).ok());  // ссылка на удалённый элемент — понятная ошибка
    EXPECT_EQ(sketch.removeOrphans(doc).value(), 2);
    EXPECT_TRUE(sketch.solve(doc).ok());
    const auto* first = doc.objectsOf<sketcher::Constraint>().front();
    ASSERT_TRUE(sketch.removeConstraint(doc, first->id()).ok());
    EXPECT_FALSE(sketch.removeConstraint(doc, core::Uuid::generate()).ok());
}

// Скорость (ЭС-5): цепочка из 150 отрезков с совпадениями, чередованием «горизонтально — вертикально» и длинами.
TEST_F(Sketch, LongChainSolvesQuickly) {
    std::vector<core::Uuid> s;
    Point2 at{0, 0};
    const int count = 150;
    for (int i = 0; i < count; ++i) {
        const Point2 next = i % 2 == 0 ? Point2{at.x + 9.0 + i % 3, at.y + 1.0} : Point2{at.x - 0.7, at.y + 11.0};
        s.push_back(line(at, next));
        at = next;
    }
    // Ограничения ставятся разом, без пересчёта после каждого, — как при открытии готового эскиза.
    document::Transaction tx(doc, "ограничения");
    const auto put = [&](ConstraintKind kind, std::vector<sketcher::Ref> refs, double value = 0.0) {
        auto c = std::make_unique<sketcher::Constraint>();
        c->kind = kind;
        c->refs = std::move(refs);
        c->value = value;
        c->name = "c";
        ASSERT_TRUE(tx.add(std::move(c)).ok());
    };
    put(ConstraintKind::Fixed, {{s[0]}});
    for (int i = 0; i < count; ++i) {
        if (i + 1 < count) put(ConstraintKind::Coincident, {{s[static_cast<std::size_t>(i)], PointRole::End}, {s[static_cast<std::size_t>(i + 1)], PointRole::Start}});
        if (i > 0) put(i % 2 == 0 ? ConstraintKind::Horizontal : ConstraintKind::Vertical, {{s[static_cast<std::size_t>(i)]}});
        if (i > 0) put(ConstraintKind::Length, {{s[static_cast<std::size_t>(i)]}}, 10.0);
    }
    tx.commit();

    const auto started = std::chrono::steady_clock::now();
    auto report = sketch.solve(doc);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().solved);
    EXPECT_EQ(report.value().variables, 4 * count);
    EXPECT_EQ(report.value().freedom, 0);
    EXPECT_NEAR(length(s.back()), 10.0, 1e-7);
    EXPECT_LT(ms, 5000.0) << "решение заняло " << ms << " мс";
    RecordProperty("solve_ms", std::to_string(ms));
    std::printf("            цепочка из 150 отрезков: %.1f мс, итераций %d\n", ms, report.value().iterations);
}
