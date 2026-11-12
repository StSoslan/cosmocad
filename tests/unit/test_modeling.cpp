// Формулы, параметрическая модель детали и её перестроение (требования 3D-1…3D-11).
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <numbers>

#include <nlohmann/json.hpp>

#include "cosmocad/geometry/OcctKernel.h"
#include "cosmocad/modeling/Expression.h"
#include "cosmocad/modeling/ModelingService.h"

using namespace cosmocad;
using namespace cosmocad::modeling;
using document::Document;
using document::DocumentKind;

namespace {

std::optional<double> noNames(std::string_view) { return std::nullopt; }

double calc(const std::string& text) {
    auto value = evaluateExpression(text, noNames);
    EXPECT_TRUE(value.ok()) << text << ": " << (value.ok() ? "" : value.error().message);
    return value.ok() ? value.value() : 0.0;
}

SketchShape rect(const std::string& x, const std::string& y, const std::string& w, const std::string& h) {
    SketchShape s;
    s.kind = SketchShape::Kind::Rectangle;
    s.values = {x, y, w, h};
    return s;
}

SketchShape circle(const std::string& x, const std::string& y, const std::string& r, bool hole = false) {
    SketchShape s;
    s.kind = SketchShape::Kind::Circle;
    s.values = {x, y, r};
    s.hole = hole;
    return s;
}

struct PartFixture : ::testing::Test {
    std::unique_ptr<geometry::IGeometryKernel> kernel = geometry::createOcctKernel();
    ModelingService service{*kernel};
    Document part{DocumentKind::Part, "Пластина"};
    core::Uuid plate, hole, fillet;

    // Пластина «ширина × 50 × толщина» с отверстием и скруглённой кромкой отверстия.
    void buildPlate() {
        ASSERT_TRUE(service.addParameter(part, "ширина", "80").ok());
        ASSERT_TRUE(service.addParameter(part, "толщина", "10").ok());
        ASSERT_TRUE(service.addParameter(part, "радиус", "ширина / 8").ok());
        auto base = service.addSketch(part, {"Контур", "XY", "0", "", {rect("0", "0", "ширина", "50")}});
        ASSERT_TRUE(base.ok()) << base.error().message;
        auto e1 = service.addExtrude(part, {base.value(), "толщина", false, false});
        ASSERT_TRUE(e1.ok());
        plate = e1.value();
        // Эскиз отверстия лежит на верхней грани пластины; вырез идёт внутрь тела.
        auto top = service.addSketch(part, {"Отверстие", "", "0", "E1/end", {circle("ширина / 2", "25", "радиус")}});
        ASSERT_TRUE(top.ok()) << top.error().message;
        auto e2 = service.addExtrude(part, {top.value(), "толщина", true, true});
        ASSERT_TRUE(e2.ok());
        hole = e2.value();
        auto f = service.addBlend(part, {{{"E1/end", "E2/side/1000"}}, "1.5", false});
        ASSERT_TRUE(f.ok());
        fillet = f.value();
    }

    double volume() {
        auto m = service.mass(part);
        EXPECT_TRUE(m.ok());
        return m.ok() ? m.value().geometry.volume : 0.0;
    }
};

}  // namespace

TEST(Expression, ArithmeticUnitsAndFunctions) {
    EXPECT_DOUBLE_EQ(calc("2 + 3 * 4"), 14.0);
    EXPECT_DOUBLE_EQ(calc("(2 + 3) * 4"), 20.0);
    EXPECT_DOUBLE_EQ(calc("-2 ^ 2"), -4.0);
    EXPECT_DOUBLE_EQ(calc("2 ^ 3 ^ 2"), 512.0);
    EXPECT_DOUBLE_EQ(calc("12,5 + 0.5"), 13.0);
    EXPECT_DOUBLE_EQ(calc("2 cm + 5 mm"), 25.0);
    EXPECT_DOUBLE_EQ(calc("1in"), 25.4);
    EXPECT_DOUBLE_EQ(calc("0.1 m"), 100.0);
    EXPECT_NEAR(calc("sin(30)"), 0.5, 1e-12);
    EXPECT_NEAR(calc("atan(1)"), 45.0, 1e-12);
    EXPECT_DOUBLE_EQ(calc("max(1, 5, 3) - min(2.5, 4)"), 2.5);
    EXPECT_DOUBLE_EQ(calc("sqrt(16) + abs(-3)"), 7.0);
}

TEST(Expression, NamesAndErrors) {
    const auto lookup = [](std::string_view name) -> std::optional<double> {
        if (name == "ширина") return 80.0;
        if (name == "b_2") return 3.0;
        return std::nullopt;
    };
    EXPECT_DOUBLE_EQ(evaluateExpression("2 * ширина + b_2 + 3 mm", lookup).value(), 166.0);
    auto unknown = evaluateExpression("ширина + высота", lookup);
    ASSERT_FALSE(unknown.ok());
    EXPECT_NE(unknown.error().message.find("высота"), std::string::npos);
    EXPECT_FALSE(evaluateExpression("1 / 0", lookup).ok());
    EXPECT_FALSE(evaluateExpression("", lookup).ok());
    EXPECT_FALSE(evaluateExpression("2 +", lookup).ok());
    EXPECT_FALSE(evaluateExpression("(2 + 3", lookup).ok());
    EXPECT_FALSE(evaluateExpression("2 $ 3", lookup).ok());
    EXPECT_FALSE(evaluateExpression("sqrt(-1)", lookup).ok());
    EXPECT_FALSE(evaluateExpression("min(1)", lookup).ok());
    auto names = expressionNames("ширина / 2 + max(a, ширина) + sin(30) + 5 mm");
    ASSERT_TRUE(names.ok());
    EXPECT_EQ(names.value(), (std::vector<std::string>{"ширина", "a"}));
    EXPECT_TRUE(isValidParameterName("ширина_2"));
    EXPECT_FALSE(isValidParameterName("2x"));
    EXPECT_FALSE(isValidParameterName("mm"));
    EXPECT_FALSE(isValidParameterName("sin"));
    EXPECT_FALSE(isValidParameterName("a b"));
}

TEST_F(PartFixture, ParametersDependOnEachOtherAndCyclesAreRejected) {
    ASSERT_TRUE(service.addParameter(part, "a", "10").ok());
    ASSERT_TRUE(service.addParameter(part, "b", "a * 2").ok());
    EXPECT_DOUBLE_EQ(service.parameterValues(part).value().at("b"), 20.0);
    // Круг: a зависит от b, b от a. Изменение отклоняется, документ не меняется.
    auto cycle = service.setParameter(part, "a", "b + 1");
    ASSERT_FALSE(cycle.ok());
    EXPECT_EQ(cycle.error().code, core::ErrorCode::InvalidState);
    EXPECT_DOUBLE_EQ(service.parameterValues(part).value().at("a"), 10.0);
    EXPECT_FALSE(service.addParameter(part, "a", "1").ok());       // повтор имени
    EXPECT_FALSE(service.addParameter(part, "c", "нет + 1").ok()); // неизвестное имя
    EXPECT_FALSE(service.addParameter(part, "mm", "1").ok());
    EXPECT_FALSE(service.removeParameter(part, "a").ok());         // используется в b
    EXPECT_TRUE(service.removeParameter(part, "b").ok());
    EXPECT_TRUE(service.removeParameter(part, "a").ok());
}

TEST_F(PartFixture, BuildsPlateWithHoleAndFillet) {
    buildPlate();
    auto report = service.recompute(part);
    ASSERT_TRUE(report.ok()) << report.error().message;
    ASSERT_EQ(report.value().features.size(), 3u);
    for (const auto& f : report.value().features) EXPECT_EQ(f.state, FeatureState::UpToDate) << f.diagnostic;
    EXPECT_EQ(report.value().recomputed, 3u);
    const double withoutFillet = 80.0 * 50 * 10 - std::numbers::pi * 100 * 10;
    EXPECT_LT(volume(), withoutFillet);
    EXPECT_GT(volume(), withoutFillet - 200.0);
    const auto names = kernel->faceNames(service.body(part));
    EXPECT_NE(std::find(names.begin(), names.end(), "F1/fillet/1"), names.end());
    EXPECT_TRUE(kernel->validate(service.body(part)).ok());
    // Масса: сталь 7,85 г/см³.
    EXPECT_NEAR(service.mass(part).value().massGrams, volume() / 1000.0 * 7.85, 1e-9);
}

// Главная проверка параметрической модели: меняем ширину — отверстие остаётся посередине,
// скругление остаётся на кромке отверстия (ссылки по именам граней не теряются).
TEST_F(PartFixture, ChangingParameterRebuildsModelAndKeepsReferences) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    for (const double width : {120.0, 60.0, 160.0}) {
        ASSERT_TRUE(service.setParameter(part, "ширина", formatNumber(width)).ok());
        auto report = service.recompute(part);
        ASSERT_TRUE(report.ok());
        for (const auto& f : report.value().features) EXPECT_EQ(f.state, FeatureState::UpToDate) << width << f.diagnostic;
        const auto wall = kernel->faceInfo(service.body(part), "E2/side/1000");
        ASSERT_TRUE(wall.ok());
        EXPECT_NEAR(wall.value().point.x, width / 2, 1e-9);
        EXPECT_NEAR(wall.value().radius, width / 8, 1e-9);
        EXPECT_TRUE(kernel->hasEdge(service.body(part), {"F1/fillet/1", "E1/end"}));
    }
}

TEST_F(PartFixture, RecomputeTouchesOnlyWhatChanged) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    // Ничего не менялось — ничего не пересчитывается.
    auto same = service.recompute(part).value();
    EXPECT_EQ(same.recomputed, 0u);
    EXPECT_EQ(same.reused, 3u);
    // Меняется только скругление: пластина и отверстие берутся готовыми.
    ASSERT_TRUE(service.setFeatureValue(part, fillet, "2").ok());
    auto last = service.recompute(part).value();
    EXPECT_EQ(last.recomputed, 1u);
    EXPECT_EQ(last.reused, 2u);
    // Меняется радиус отверстия: пластина берётся готовой.
    ASSERT_TRUE(service.setParameter(part, "радиус", "6").ok());
    auto middle = service.recompute(part).value();
    EXPECT_EQ(middle.recomputed, 2u);
    EXPECT_EQ(middle.reused, 1u);
    // Отмена возвращает прежнюю модель.
    const double before = volume();
    ASSERT_TRUE(part.undo());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NE(volume(), before);
    ASSERT_TRUE(part.redo());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_DOUBLE_EQ(volume(), before);
}

TEST_F(PartFixture, FailedFeatureIsReportedAndDoesNotStopOthers) {
    buildPlate();
    // Скругление больше, чем позволяет деталь.
    ASSERT_TRUE(service.setFeatureValue(part, fillet, "500").ok());
    auto report = service.recompute(part).value();
    EXPECT_EQ(report.features[0].state, FeatureState::UpToDate);
    EXPECT_EQ(report.features[1].state, FeatureState::UpToDate);
    EXPECT_EQ(report.features[2].state, FeatureState::Failed);
    EXPECT_FALSE(report.features[2].diagnostic.empty());
    // Тело есть: пластина с отверстием, без скругления.
    EXPECT_NEAR(volume(), 80.0 * 50 * 10 - std::numbers::pi * 100 * 10, 1e-3);
    EXPECT_EQ(service.status(part).size(), 3u);
}

TEST_F(PartFixture, SuppressReorderAndRemove) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    // Отключаем отверстие: скруглению не на что сослаться — оно сообщает о сбое.
    ASSERT_TRUE(service.suppressFeature(part, hole, true).ok());
    auto report = service.recompute(part).value();
    EXPECT_EQ(report.features[1].state, FeatureState::Suppressed);
    EXPECT_EQ(report.features[2].state, FeatureState::Failed);
    EXPECT_NE(report.features[2].diagnostic.find("E2/side/1000"), std::string::npos);
    EXPECT_NEAR(volume(), 80.0 * 50 * 10, 1e-6);
    ASSERT_TRUE(service.suppressFeature(part, hole, false).ok());

    // Скругление раньше отверстия: кромки ещё нет — сбой с объяснением; обратно — всё в порядке.
    ASSERT_TRUE(service.reorderFeature(part, fillet, 1).ok());
    EXPECT_EQ(service.features(part)[1]->id(), fillet);
    EXPECT_EQ(service.recompute(part).value().features[1].state, FeatureState::Failed);
    ASSERT_TRUE(service.reorderFeature(part, fillet, 2).ok());
    const auto restored = service.recompute(part).value();
    for (const auto& f : restored.features) EXPECT_EQ(f.state, FeatureState::UpToDate);
    EXPECT_FALSE(service.reorderFeature(part, fillet, 3).ok());

    // Удаление операции убирает и её эскиз.
    const std::size_t sketches = part.objectsOf<Sketch>().size();
    ASSERT_TRUE(service.removeFeature(part, fillet).ok());
    ASSERT_TRUE(service.removeFeature(part, hole).ok());
    EXPECT_EQ(part.objectsOf<Sketch>().size(), sketches - 1);
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), 80.0 * 50 * 10, 1e-6);
}

TEST_F(PartFixture, SketchWithHoleAndSeveralFigures) {
    // Одна фигура с отверстием и отдельный выступ — одна операция.
    auto sketch = service.addSketch(part, {"", "XY", "0", "",
                                           {rect("0", "0", "40", "40"), circle("20", "20", "5", true),
                                            rect("40", "10", "20", "20")}});
    ASSERT_TRUE(sketch.ok()) << sketch.error().message;
    ASSERT_TRUE(service.addExtrude(part, {sketch.value(), "10", false, false}).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), (1600 - std::numbers::pi * 25 + 400) * 10, 1e-3);
    // Отверстие вне фигур и пустой эскиз отклоняются при вводе.
    EXPECT_FALSE(service.addSketch(part, {"", "XY", "0", "", {rect("0", "0", "10", "10"), circle("50", "50", "2", true)}}).ok());
    EXPECT_FALSE(service.addSketch(part, {"", "XY", "0", "", {}}).ok());
    EXPECT_FALSE(service.addSketch(part, {"", "XY", "0", "", {rect("0", "0", "-10", "10")}}).ok());
    EXPECT_FALSE(service.addSketch(part, {"", "AB", "0", "", {rect("0", "0", "10", "10")}}).ok());
}

TEST_F(PartFixture, RevolveAndPlanes) {
    // Втулка: прямоугольник 10…20 по X, высота 30, вращение вокруг оси Y эскиза в плоскости XZ.
    auto sketch = service.addSketch(part, {"", "XZ", "0", "", {rect("10", "0", "10", "30")}});
    ASSERT_TRUE(sketch.ok());
    ASSERT_TRUE(service.addRevolve(part, {sketch.value(), "Y", "360", false}).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), std::numbers::pi * (400 - 100) * 30, 1e-2);
    // Ось Y эскиза в плоскости XZ — это ось Z мира: центр масс на высоте 15.
    EXPECT_NEAR(service.mass(part).value().geometry.centerOfMass.z, 15.0, 1e-6);
    EXPECT_FALSE(service.addRevolve(part, {sketch.value(), "Z", "360", false}).ok());
}

TEST_F(PartFixture, UpdateSketchKeepsTagsOfExistingFigures) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    const auto* holeFeature = part.findAs<Feature>(hole);
    auto shapes = part.findAs<Sketch>(holeFeature->sketchId)->shapes;
    shapes.push_back(circle("10", "10", "3"));  // второе отверстие; первое сохраняет метку
    ASSERT_TRUE(service.updateSketch(part, holeFeature->sketchId, shapes).ok());
    auto report = service.recompute(part).value();
    for (const auto& f : report.features) EXPECT_EQ(f.state, FeatureState::UpToDate) << f.diagnostic;
    const auto names = kernel->faceNames(service.body(part));
    EXPECT_NE(std::find(names.begin(), names.end(), "E2/side/2000"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "F1/fillet/1"), names.end());
}

// Массивы (требование 3D-5): ряд и круг отверстий, число копий — параметр.
TEST_F(PartFixture, LinearAndCircularPatternsWithCountAsParameter) {
    ASSERT_TRUE(service.addParameter(part, "n", "4").ok());
    auto base = service.addSketch(part, {"", "XY", "0", "", {rect("0", "0", "100", "100")}});
    ASSERT_TRUE(service.addExtrude(part, {base.value(), "10", false, false}).ok());
    auto holeSketch = service.addSketch(part, {"", "", "0", "E1/end", {circle("20", "20", "3")}});
    auto holes = service.addExtrude(part, {holeSketch.value(), "10", true, true});
    ASSERT_TRUE(holes.ok());
    const double plate = 100.0 * 100 * 10, one = std::numbers::pi * 9 * 10;

    // Ряд из n отверстий с шагом 20 мм по X.
    Pattern row;
    row.kind = Pattern::Kind::Linear;
    row.count = "n";
    row.stepX = "20";
    ASSERT_TRUE(service.setPattern(part, holes.value(), row).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), plate - 4 * one, 1e-3);
    // У каждой копии своя грань со своим именем; четвёртое отверстие — на x = 80.
    const auto last = kernel->faceInfo(service.body(part), "E2/side/3001000");
    ASSERT_TRUE(last.ok());
    EXPECT_NEAR(last.value().point.x, 80.0, 1e-9);
    // Число копий — параметр: меняем n, отверстий становится три.
    ASSERT_TRUE(service.setParameter(part, "n", "3").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), plate - 3 * one, 1e-3);
    EXPECT_FALSE(service.removeParameter(part, "n").ok());  // параметр занят массивом

    // Шесть отверстий по кругу вокруг середины плиты.
    Pattern ring;
    ring.kind = Pattern::Kind::Circular;
    ring.count = "6";
    ring.centerX = "50";
    ring.centerY = "50";
    ASSERT_TRUE(service.setPattern(part, holes.value(), ring).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), plate - 6 * one, 1e-3);
    // Исходное отверстие в (20; 20); четвёртое — напротив, в (80; 80).
    const auto opposite = kernel->faceInfo(service.body(part), "E2/side/3001000");
    ASSERT_TRUE(opposite.ok());
    EXPECT_NEAR(opposite.value().point.x, 80.0, 1e-9);
    EXPECT_NEAR(opposite.value().point.y, 80.0, 1e-9);
    // Неполный круг: три отверстия на 90° — крайние в начале и в конце дуги.
    ring.count = "3";
    ring.angle = "90";
    ASSERT_TRUE(service.setPattern(part, holes.value(), ring).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    const auto end = kernel->faceInfo(service.body(part), "E2/side/2001000");
    ASSERT_TRUE(end.ok());
    EXPECT_NEAR(end.value().point.x, 80.0, 1e-9);  // (20; 20) повёрнута на 90° вокруг (50; 50)
    EXPECT_NEAR(end.value().point.y, 20.0, 1e-9);

    // Дробное или нулевое число копий — сбой операции с объяснением, тело остаётся плитой.
    ring.count = "2.5";
    ASSERT_TRUE(service.setPattern(part, holes.value(), ring).ok());
    auto report = service.recompute(part).value();
    EXPECT_EQ(report.features[1].state, FeatureState::Failed);
    EXPECT_NE(report.features[1].diagnostic.find("целым"), std::string::npos);
    EXPECT_NEAR(volume(), plate, 1e-6);
    // Неизвестное имя в формуле отклоняется сразу; массив у скругления невозможен.
    ring.count = "нет_такого";
    EXPECT_FALSE(service.setPattern(part, holes.value(), ring).ok());

    // Массив сохраняется в файле и убирается.
    ASSERT_TRUE(service.setPattern(part, holes.value(), row).ok());
    document::ObjectFactory factory;
    registerTypes(factory);
    auto loaded = Document::fromJson(part.toJson(), factory);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    ModelingService other{*kernel};
    ASSERT_TRUE(other.recompute(*loaded.value()).ok());
    EXPECT_NEAR(other.mass(*loaded.value()).value().geometry.volume, plate - 3 * one, 1e-3);
    ASSERT_TRUE(service.setPattern(part, holes.value(), Pattern{}).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), plate - one, 1e-3);
}

// Откат дерева построения к любому шагу (требование 3D-7).
TEST_F(PartFixture, RollbackShowsModelAtEarlierStep) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    const double full = volume();
    // Откат после первой операции: видна сплошная пластина, отверстие и скругление ждут.
    ASSERT_TRUE(service.rollTo(part, plate).ok());
    auto report = service.recompute(part).value();
    EXPECT_NEAR(volume(), 80.0 * 50 * 10, 1e-6);
    EXPECT_EQ(report.features[0].state, FeatureState::UpToDate);
    EXPECT_EQ(report.features[1].state, FeatureState::Suppressed);
    EXPECT_EQ(report.features[1].diagnostic, "после отката");
    EXPECT_FALSE(part.findAs<Feature>(hole)->suppressed);  // сама операция не изменена
    // Откат после отверстия; затем снятие отката возвращает полную модель.
    ASSERT_TRUE(service.rollTo(part, hole).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), 80.0 * 50 * 10 - std::numbers::pi * 100 * 10, 1e-3);
    ASSERT_TRUE(service.rollTo(part, core::Uuid()).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_DOUBLE_EQ(volume(), full);
    // Откат хранится в файле; удаление операции, на которой он стоит, снимает его.
    ASSERT_TRUE(service.rollTo(part, hole).ok());
    document::ObjectFactory factory;
    registerTypes(factory);
    auto loaded = Document::fromJson(part.toJson(), factory);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value()->objectsOf<PartProperties>().front()->rollbackAfter, hole);
    ASSERT_TRUE(service.removeFeature(part, fillet).ok());
    ASSERT_TRUE(service.removeFeature(part, hole).ok());
    EXPECT_TRUE(part.objectsOf<PartProperties>().front()->rollbackAfter.isNil());
    EXPECT_FALSE(service.rollTo(part, hole).ok());  // такой операции больше нет
}

TEST_F(PartFixture, PartSurvivesSaveAndLoad) {
    buildPlate();
    ASSERT_TRUE(service.setPartProperties(part, "КСМ.003.001", "Сталь 45", 7.81).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    const double before = volume();
    const auto namesBefore = kernel->faceNames(service.body(part));

    document::ObjectFactory factory;
    registerTypes(factory);
    auto loaded = Document::fromJson(part.toJson(), factory);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    ModelingService other{*kernel};
    ASSERT_TRUE(other.recompute(*loaded.value()).ok());
    // То же тело и те же имена граней: построение воспроизводимо (требование 3D-9).
    EXPECT_DOUBLE_EQ(other.mass(*loaded.value()).value().geometry.volume, before);
    EXPECT_EQ(kernel->faceNames(other.body(*loaded.value())), namesBefore);
    EXPECT_EQ(loaded.value()->objectsOf<PartProperties>().front()->material, "Сталь 45");
    EXPECT_NEAR(other.mass(*loaded.value()).value().massGrams, before / 1000.0 * 7.81, 1e-9);
}

TEST_F(PartFixture, RecomputeCanBeCancelledAndWrongDocumentKindIsRejected) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    const double before = volume();
    ASSERT_TRUE(service.setParameter(part, "ширина", "100").ok());
    std::atomic<bool> cancel{true};
    auto report = service.recompute(part, &cancel);
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().cancelled);
    EXPECT_DOUBLE_EQ(volume(), before);  // прежнее тело осталось
    Document drawing{DocumentKind::Drawing, "Чертёж"};
    EXPECT_FALSE(service.addParameter(drawing, "a", "1").ok());
    EXPECT_FALSE(service.recompute(drawing).ok());
}

// Зеркальное отражение (требование 3D-5): к телу добавляется его зеркальная копия.
TEST_F(PartFixture, MirrorAddsCopyAndFollowsParameters) {
    buildPlate();
    ASSERT_TRUE(service.recompute(part).ok());
    const double single = volume();

    // Плоскость YZ проходит по левому торцу пластины: получается пластина вдвое длиннее с двумя отверстиями.
    auto mirror = service.addMirror(part, {"YZ", "0"});
    ASSERT_TRUE(mirror.ok()) << mirror.error().message;
    auto report = service.recompute(part);
    ASSERT_TRUE(report.ok());
    for (const auto& status : report.value().features) EXPECT_EQ(status.state, FeatureState::UpToDate) << status.diagnostic;
    EXPECT_NEAR(volume(), 2.0 * single, 1e-6);
    const auto names = kernel->faceNames(service.body(part));
    EXPECT_NE(std::find(names.begin(), names.end(), "M1/mirror/E2/side/1000"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "E2/side/1000"), names.end());
    auto box = kernel->bounds(service.body(part));
    ASSERT_TRUE(box.ok());
    EXPECT_NEAR(box.value().min.x, -80.0, 1e-6);
    EXPECT_NEAR(box.value().max.x, 80.0, 1e-6);

    // Копия следует за параметром: пластина шире — и отражение шире.
    ASSERT_TRUE(service.setParameter(part, "ширина", "100").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(kernel->bounds(service.body(part)).value().min.x, -100.0, 1e-6);

    // Плоскость по середине симметричной пластины: копия совпадает с исходным телом, объём прежний.
    ASSERT_TRUE(service.setFeatureValue(part, mirror.value(), "ширина / 2").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    const double symmetric = volume();
    ASSERT_TRUE(service.suppressFeature(part, mirror.value(), true).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(symmetric, volume(), 1e-6);

    EXPECT_FALSE(service.addMirror(part, {"AB", "0"}).ok());
    EXPECT_FALSE(service.addMirror(part, {"XY", "нет_такого"}).ok());
}

// Исполнения детали (требование СБ-4): именованные наборы значений параметров.
TEST_F(PartFixture, ConfigurationsStoreAndApplyParameterSets) {
    buildPlate();
    ASSERT_TRUE(service.setPartProperties(part, "КСМ.001", "Сталь 45", 7.85).ok());
    ASSERT_TRUE(service.recompute(part).ok());
    const double narrow = volume();
    ASSERT_TRUE(service.saveConfiguration(part, "Узкая").ok());

    ASSERT_TRUE(service.setParameter(part, "ширина", "120").ok());
    ASSERT_TRUE(service.setParameter(part, "толщина", "16").ok());
    ASSERT_TRUE(service.saveConfiguration(part, "Широкая", "КСМ.001-01").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    const double wide = volume();
    EXPECT_GT(wide, narrow * 2.0);
    EXPECT_EQ(service.configurations(part), (std::vector<std::string>{"Узкая", "Широкая"}));

    // Применение меняет все параметры одним шагом отмены; обозначение исполнения ставится, если задано.
    ASSERT_TRUE(service.applyConfiguration(part, "Узкая").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), narrow, 1e-6);
    EXPECT_EQ(part.objectsOf<PartProperties>().front()->designation, "КСМ.001");
    ASSERT_TRUE(service.applyConfiguration(part, "Широкая").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), wide, 1e-6);
    EXPECT_EQ(part.objectsOf<PartProperties>().front()->designation, "КСМ.001-01");
    ASSERT_TRUE(part.undo());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), narrow, 1e-6);

    // Повтор имени заменяет исполнение; неизвестное имя и пустое имя — отказ; удаление.
    ASSERT_TRUE(service.setParameter(part, "ширина", "90").ok());
    ASSERT_TRUE(service.saveConfiguration(part, "Узкая").ok());
    EXPECT_EQ(service.configurations(part).size(), 2u);
    EXPECT_FALSE(service.applyConfiguration(part, "Нет такого").ok());
    EXPECT_FALSE(service.saveConfiguration(part, "").ok());
    ASSERT_TRUE(service.removeConfiguration(part, "Широкая").ok());
    EXPECT_EQ(service.configurations(part), (std::vector<std::string>{"Узкая"}));
}

// Протяжка по траектории (требование 3D-2).
TEST_F(PartFixture, SweepFollowsPathWithBends) {
    // Пруток ⌀10 идёт вверх на 50 мм — тот же цилиндр, что дало бы выдавливание.
    auto ring = service.addSketch(part, {"Сечение", "XY", "0", "", {circle("0", "0", "5")}});
    ASSERT_TRUE(ring.ok());
    auto straight = service.addSweep(part, {ring.value(), {{"0", "0", "50"}}, "0", false});
    ASSERT_TRUE(straight.ok()) << straight.error().message;
    auto report = service.recompute(part);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().features.back().state, FeatureState::UpToDate) << report.value().features.back().diagnostic;
    const double area = std::numbers::pi * 25.0;
    EXPECT_NEAR(volume(), area * 50.0, 1e-3);
    ASSERT_TRUE(service.removeFeature(part, straight.value()).ok());

    // Гнутый пруток: вверх 50, гиб радиусом 20, вбок 60. Объём — площадь сечения на длину осевой линии.
    auto section = service.addSketch(part, {"Сечение", "XY", "0", "", {circle("0", "0", "5")}});
    ASSERT_TRUE(section.ok());
    ASSERT_TRUE(service.addParameter(part, "гиб", "20").ok());
    auto bent = service.addSweep(part, {section.value(), {{"0", "0", "50"}, {"60", "0", "50"}}, "гиб", false});
    ASSERT_TRUE(bent.ok()) << bent.error().message;
    report = service.recompute(part);
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().features.back().state, FeatureState::UpToDate) << report.value().features.back().diagnostic;
    const auto length = [](double r) { return (50.0 - r) + (60.0 - r) + std::numbers::pi / 2.0 * r; };
    EXPECT_NEAR(volume(), area * length(20.0), area * length(20.0) * 1e-4);
    auto box = kernel->bounds(service.body(part));
    ASSERT_TRUE(box.ok());
    EXPECT_NEAR(box.value().max.x, 60.0, 1e-6);
    EXPECT_NEAR(box.value().max.z, 55.0, 1e-6);

    // Радиус гиба — параметр: изменили — пруток перестроился.
    ASSERT_TRUE(service.setParameter(part, "гиб", "10").ok());
    ASSERT_TRUE(service.recompute(part).ok());
    EXPECT_NEAR(volume(), area * length(10.0), area * length(10.0) * 1e-4);

    // Излом без радиуса и слишком большой радиус — сбой операции с объяснением, а не падение.
    ASSERT_TRUE(service.setParameter(part, "гиб", "0").ok());
    report = service.recompute(part);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().features.back().state, FeatureState::Failed);
    EXPECT_NE(report.value().features.back().diagnostic.find("радиус гиба"), std::string::npos);
    ASSERT_TRUE(service.setParameter(part, "гиб", "70").ok());
    report = service.recompute(part);
    EXPECT_EQ(report.value().features.back().state, FeatureState::Failed);
    // Пустая траектория и неизвестный параметр отклоняются сразу.
    EXPECT_FALSE(service.addSweep(part, {section.value(), {}, "0", false}).ok());
    EXPECT_FALSE(service.addSweep(part, {section.value(), {{"0", "0", "нет_такого"}}, "0", false}).ok());
}
