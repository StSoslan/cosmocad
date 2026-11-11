// Сборки, виды чертежа по модели и сквозной пример «деталь → сборка → чертёж → файл».
#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <filesystem>
#include <numbers>

#include "AppContext.h"

using namespace cosmocad;
namespace fs = std::filesystem;
using assembly::MateKind;
using document::DocumentKind;
using geometry::Placement;

namespace {

fs::path resources() { return COSMOCAD_TEST_RESOURCES; }

struct Demo : ::testing::Test {
    std::unique_ptr<app::AppContext> context = app::AppContext::create(resources()).take();
    app::AppContext::DemoModel demo;

    void SetUp() override {
        auto created = context->createDemoModel();
        ASSERT_TRUE(created.ok()) << created.error().message;
        demo = created.value();
    }

    const assembly::Component* component(std::size_t index) {
        return demo.assembly->objectsOf<assembly::Component>().at(index);
    }
};

}  // namespace

TEST_F(Demo, PartsAreBuiltWithoutFailures) {
    for (auto* part : {demo.support, demo.pin}) {
        for (const auto& status : context->modeling().status(*part))
            EXPECT_EQ(status.state, modeling::FeatureState::UpToDate) << status.diagnostic;
        EXPECT_TRUE(context->kernel().validate(context->modeling().body(*part)).ok());
    }
    // Плита 80×50×10 минус 4 отверстия ⌀6, плюс бобышка ⌀36×15, минус отверстие ⌀24×25 —
    // с поправкой на скругление и фаску объём близок к этому числу.
    const double expected = 80.0 * 50 * 10 - 4 * std::numbers::pi * 9 * 10 + std::numbers::pi * 18 * 18 * 15 -
                            std::numbers::pi * 144 * 25;
    EXPECT_NEAR(context->modeling().mass(*demo.support).value().geometry.volume, expected, 400.0);
}

TEST_F(Demo, MatesPutPinIntoBore) {
    const auto* pin = component(1);
    EXPECT_TRUE(component(0)->fixed);
    EXPECT_FALSE(pin->fixed);
    // Ось пальца совпала с осью отверстия (40; 25), низ пальца — на уровне низа плиты.
    const auto origin = pin->placement.apply({0, 0, 0});
    EXPECT_NEAR(origin.x, 40.0, 1e-6);
    EXPECT_NEAR(origin.y, 25.0, 1e-6);
    EXPECT_NEAR(origin.z, 0.0, 1e-6);
    const auto axis = pin->placement.rotate({0, 0, 1});
    EXPECT_NEAR(std::abs(axis.z), 1.0, 1e-9);
    // Повторный расчёт ничего не меняет и не создаёт шага отмены.
    const std::string before = demo.assembly->undoName();
    auto again = context->assembly().solve(*demo.assembly);
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(again.value().solved);
    EXPECT_EQ(again.value().passes, 1);
    EXPECT_EQ(demo.assembly->undoName(), before);
}

TEST_F(Demo, InterferenceIsFoundOnlyWhenBodiesOverlap) {
    auto clear = context->assembly().checkInterference(*demo.assembly);
    ASSERT_TRUE(clear.ok()) << clear.error().message;
    EXPECT_TRUE(clear.value().empty());  // палец ⌀23,9 в отверстии ⌀24
    // Сдвигаем палец вбок на 5 мм: он врезается в стенку отверстия.
    Placement moved = Placement::translation(5, 0, 0) * component(1)->placement;
    ASSERT_TRUE(context->assembly().setPlacement(*demo.assembly, component(1)->id(), moved).ok());
    auto hit = context->assembly().checkInterference(*demo.assembly);
    ASSERT_TRUE(hit.ok());
    ASSERT_EQ(hit.value().size(), 1u);
    EXPECT_GT(hit.value()[0].volume, 100.0);
    // Сопряжения возвращают палец на место.
    ASSERT_TRUE(context->assembly().solve(*demo.assembly).value().solved);
    EXPECT_TRUE(context->assembly().checkInterference(*demo.assembly).value().empty());
}

TEST_F(Demo, DistanceMateAndConflictReport) {
    // Третий компонент — ещё один палец на расстоянии 30 мм над бобышкой.
    auto extra = context->assembly().addComponent(*demo.assembly, demo.pin->id(), Placement::translation(0, 0, 200));
    ASSERT_TRUE(extra.ok());
    auto distance = context->assembly().addMate(
        *demo.assembly, {MateKind::Distance, {component(0)->id(), "E2/end"}, {extra.value(), "E1/start"}, 30.0, false});
    ASSERT_TRUE(distance.ok()) << distance.error().message;
    ASSERT_TRUE(context->assembly().solve(*demo.assembly).value().solved);
    // Верх бобышки на высоте 25; низ пальца должен быть на 55 и смотреть вниз.
    const auto* placed = demo.assembly->findAs<assembly::Component>(extra.value());
    EXPECT_NEAR(placed->placement.apply({0, 0, 0}).z, 55.0, 1e-6);
    EXPECT_NEAR(placed->placement.rotate({0, 0, 1}).z, 1.0, 1e-9);

    // Противоречие: тот же торец должен быть ещё и на расстоянии 10.
    auto conflict = context->assembly().addMate(
        *demo.assembly, {MateKind::Distance, {component(0)->id(), "E2/end"}, {extra.value(), "E1/start"}, 10.0, false});
    ASSERT_TRUE(conflict.ok());
    auto report = context->assembly().solve(*demo.assembly);
    ASSERT_TRUE(report.ok());
    EXPECT_FALSE(report.value().solved);
    EXPECT_FALSE(report.value().conflicts.empty());

    // Неподходящие грани отклоняются сразу.
    EXPECT_FALSE(context->assembly()
                     .addMate(*demo.assembly, {MateKind::Concentric, {component(0)->id(), "E1/end"},
                                               {extra.value(), "E1/side/1000"}, 0.0, false})
                     .ok());
    EXPECT_FALSE(context->assembly()
                     .addMate(*demo.assembly, {MateKind::Coincident, {component(0)->id(), "нет такой грани"},
                                               {extra.value(), "E1/start"}, 0.0, false})
                     .ok());
    // Удаление компонента убирает его сопряжения.
    ASSERT_TRUE(context->assembly().removeComponent(*demo.assembly, extra.value()).ok());
    EXPECT_EQ(demo.assembly->objectsOf<assembly::Mate>().size(), 2u);
}

TEST_F(Demo, BillOfMaterialsCountsEqualParts) {
    ASSERT_TRUE(context->assembly().addComponent(*demo.assembly, demo.pin->id(), Placement::translation(0, 0, 300)).ok());
    auto bom = context->assembly().billOfMaterials(*demo.assembly);
    ASSERT_TRUE(bom.ok());
    ASSERT_EQ(bom.value().size(), 2u);
    EXPECT_EQ(bom.value()[0].designation, "КСМ.003.001");
    EXPECT_EQ(bom.value()[0].quantity, 1);
    EXPECT_EQ(bom.value()[1].name, "Палец");
    EXPECT_EQ(bom.value()[1].quantity, 2);
    EXPECT_GT(bom.value()[1].massGrams, 100.0);
    // Сборку нельзя вставить в саму себя, чертёж — не деталь.
    EXPECT_FALSE(context->assembly().addComponent(*demo.assembly, demo.assembly->id(), Placement{}).ok());
    EXPECT_FALSE(context->assembly().addComponent(*demo.assembly, demo.drawing->id(), Placement{}).ok());
}

TEST_F(Demo, MateFollowsModelChange) {
    // Меняем длину плиты: отверстие уезжает в новую середину, палец после расчёта едет за ним.
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "120").ok());
    auto report = context->assembly().solve(*demo.assembly);
    ASSERT_TRUE(report.ok()) << report.error().message;
    EXPECT_TRUE(report.value().solved);
    EXPECT_NEAR(component(1)->placement.apply({0, 0, 0}).x, 60.0, 1e-6);
}

TEST_F(Demo, DrawingViewsComeFromModelAndPassStandardsCheck) {
    const auto views = demo.drawing->objectsOf<drawing::ModelView>();
    ASSERT_EQ(views.size(), 3u);
    // Вид сверху: контур 80×50, 4 крепёжных отверстия, отверстие и бобышка — окружности.
    int circles = 0;
    geometry::BBox2 box;
    for (const auto& id : views[1]->entities) {
        if (demo.drawing->findAs<drawing::CircleEntity>(id)) ++circles;
        if (const auto* line = demo.drawing->findAs<drawing::LineEntity>(id)) {
            box.expand(line->start);
            box.expand(line->end);
        }
    }
    EXPECT_GE(circles, 6);
    EXPECT_NEAR(box.width(), 80.0, 1e-6);
    EXPECT_NEAR(box.height(), 50.0, 1e-6);
    EXPECT_NEAR(box.min.x, 45.0, 1e-6);  // середина вида — в заданной точке (85; 170)
    // На виде спереди есть штриховые линии невидимого контура.
    int hidden = 0;
    for (const auto& id : views[0]->entities)
        if (const auto* e = demo.drawing->findAs<drawing::Entity>(id); e && e->lineTypeId == std::string("dashed")) ++hidden;
    EXPECT_GT(hidden, 0);
    auto violations = context->standards().check(*demo.drawing);
    ASSERT_TRUE(violations.ok());
    for (const auto& v : violations.value()) ADD_FAILURE() << v.ruleId << ": " << v.message;
}

TEST_F(Demo, ViewsUpdateAfterModelChangeInOneUndoStep) {
    const auto before = demo.drawing->objectsOf<drawing::ModelView>()[1]->entities;
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "100").ok());
    auto updated = context->updateModelViews(*demo.drawing);
    ASSERT_TRUE(updated.ok()) << updated.error().message;
    EXPECT_EQ(updated.value(), 3);
    const auto* top = demo.drawing->objectsOf<drawing::ModelView>()[1];
    geometry::BBox2 box;
    for (const auto& id : top->entities)
        if (const auto* line = demo.drawing->findAs<drawing::LineEntity>(id)) {
            box.expand(line->start);
            box.expand(line->end);
        }
    EXPECT_NEAR(box.width(), 100.0, 1e-6);
    EXPECT_NEAR(box.min.x, 45.0, 1e-6);  // левый край остался на месте
    for (const auto& id : before) EXPECT_EQ(demo.drawing->find(id), nullptr);  // старые линии убраны
    // Отмена трёх обновлений возвращает прежние линии.
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(demo.drawing->undo());
    for (const auto& id : before) EXPECT_NE(demo.drawing->find(id), nullptr);
}

// Размеры чертежа следуют за моделью (требование ЧЕР-6).
TEST_F(Demo, DimensionsFollowModelViews) {
    const auto linear = [&](double value) -> const drawing::LinearDimension* {
        for (const auto* d : demo.drawing->objectsOf<drawing::LinearDimension>())
            if (std::abs(d->value() - value) < 1e-6) return d;
        return nullptr;
    };
    ASSERT_NE(linear(80.0), nullptr);  // длина плиты на виде сверху
    ASSERT_NE(linear(50.0), nullptr);  // ширина
    ASSERT_NE(linear(25.0), nullptr);  // общая высота на виде спереди
    const core::Uuid lengthId = linear(80.0)->id(), heightId = linear(25.0)->id();

    // Длина 80 → 100, диаметр отверстия 24 → 20, бобышка выше на 5.
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "100").ok());
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "диаметр", "20").ok());
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "высота_бобышки", "20").ok());
    ASSERT_TRUE(context->updateModelViews(*demo.drawing).ok());
    EXPECT_EQ(context->lastViewUpdate().lostDimensions, 0);
    EXPECT_GE(context->lastViewUpdate().movedDimensions, 3);
    EXPECT_NEAR(demo.drawing->findAs<drawing::LinearDimension>(lengthId)->value(), 100.0, 1e-6);
    EXPECT_NEAR(demo.drawing->findAs<drawing::LinearDimension>(heightId)->value(), 30.0, 1e-6);
    ASSERT_NE(linear(50.0), nullptr);  // ширина не менялась — размер прежний
    const auto* diameter = demo.drawing->objectsOf<drawing::RadialDimension>().front();
    EXPECT_NEAR(diameter->value(), 20.0, 1e-6);
    EXPECT_NEAR(diameter->center.x, 95.0, 1e-6);  // центр уехал вместе с серединой плиты

    // Размер, поставленный мимо вида, не трогается.
    drawing::IDrawingService& drawingService = context->drawing();
    auto loose = drawingService.addLinearDimension(*demo.drawing, {30, 100}, {60, 100}, 5.0);
    ASSERT_TRUE(loose.ok());
    // Модель меняется так, что точек на виде становится больше: бобышка отключена, отверстие не строится.
    ASSERT_TRUE(context->modeling().suppressFeature(*demo.support, context->modeling().features(*demo.support)[1]->id(), true).ok());
    ASSERT_TRUE(context->updateModelViews(*demo.drawing).ok());
    EXPECT_GT(context->lastViewUpdate().lostDimensions, 0);
    int marked = 0;
    for (const auto* e : demo.drawing->objectsOf<drawing::Entity>()) marked += e->name == "Размер: привязка потеряна" ? 1 : 0;
    EXPECT_EQ(marked, context->lastViewUpdate().lostDimensions);
    EXPECT_NEAR(demo.drawing->findAs<drawing::LinearDimension>(loose.value())->value(), 30.0, 1e-9);
}

// Зазор между компонентами (требование СБ-6).
TEST_F(Demo, ClearanceBetweenComponents) {
    // Палец ⌀23,9 в отверстии ⌀24: зазор на сторону 0,05 мм.
    auto gap = context->assembly().clearance(*demo.assembly, component(0)->id(), component(1)->id());
    ASSERT_TRUE(gap.ok()) << gap.error().message;
    EXPECT_NEAR(gap.value(), 0.05, 1e-6);
    // Третий компонент в стороне: расстояние считается по ближайшим точкам.
    auto far = context->assembly().addComponent(*demo.assembly, demo.pin->id(), Placement::translation(200, 25, 0));
    ASSERT_TRUE(far.ok());
    // Плита кончается на x = 80, палец радиуса 11,95 стоит на x = 200.
    EXPECT_NEAR(context->assembly().clearance(*demo.assembly, component(0)->id(), far.value()).value(), 200.0 - 11.95 - 80.0, 1e-6);
    EXPECT_FALSE(context->assembly().clearance(*demo.assembly, far.value(), far.value()).ok());
}

// Сборка в сборке (требование СБ-1).
TEST_F(Demo, AssemblyCanBeUsedInsideAnotherAssembly) {
    auto top = context->documents().createDocument(DocumentKind::Assembly, "Изделие");
    ASSERT_TRUE(top.ok());
    document::Document& product = *top.value();
    auto first = context->assembly().addComponent(product, demo.assembly->id(), Placement{});
    ASSERT_TRUE(first.ok()) << first.error().message;
    auto second = context->assembly().addComponent(product, demo.assembly->id(), Placement::translation(150, 0, 0));
    ASSERT_TRUE(second.ok());
    // Два узла по два тела: объём изделия — два объёма узла.
    auto bodies = context->bodiesOf(product);
    ASSERT_TRUE(bodies.ok());
    ASSERT_EQ(bodies.value().size(), 2u);
    const double unit = context->modeling().mass(*demo.support).value().geometry.volume +
                        context->modeling().mass(*demo.pin).value().geometry.volume;
    EXPECT_NEAR(context->kernel().massProperties(bodies.value()[0]).value().volume, unit, 1e-3);
    // Состав: подсборка — одна строка, две штуки, масса — сумма масс её деталей.
    auto bom = context->assembly().billOfMaterials(product);
    ASSERT_TRUE(bom.ok());
    ASSERT_EQ(bom.value().size(), 1u);
    EXPECT_EQ(bom.value()[0].name, "Опора в сборе");
    EXPECT_EQ(bom.value()[0].quantity, 2);
    const double unitMass = context->modeling().mass(*demo.support).value().massGrams +
                            context->modeling().mass(*demo.pin).value().massGrams;
    EXPECT_NEAR(bom.value()[0].massGrams, unitMass, 1e-6);
    EXPECT_TRUE(context->assembly().checkInterference(product).value().empty());
    // Изменение детали видно и в изделии.
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "толщина", "12").ok());
    EXPECT_GT(context->kernel().massProperties(context->bodiesOf(product).value()[0]).value().volume, unit + 1000.0);
    // Вложить изделие в его собственный узел нельзя: получился бы круг.
    const std::size_t before = demo.assembly->objectsOf<assembly::Component>().size();
    auto loop = context->assembly().addComponent(*demo.assembly, product.id(), Placement{});
    ASSERT_FALSE(loop.ok());
    EXPECT_NE(loop.error().message.find("сама в себя"), std::string::npos);
    EXPECT_EQ(demo.assembly->objectsOf<assembly::Component>().size(), before);
    // Чертёж компонентом быть не может.
    EXPECT_FALSE(context->assembly().addComponent(product, demo.drawing->id(), Placement{}).ok());
}

TEST_F(Demo, ProjectWith3DSurvivesSaveAndLoad) {
    const fs::path path = fs::temp_directory_path() / "cosmocad_demo3d.cosmocad";
    fs::remove_all(path);
    const double volume = context->modeling().mass(*demo.support).value().geometry.volume;
    const Placement pinPlace = component(1)->placement;
    ASSERT_TRUE(context->documents().saveProject(path).ok());

    auto other = app::AppContext::create(resources()).take();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    ASSERT_TRUE(other->rebuildAll().ok());
    document::Document* support = nullptr;
    document::Document* assemblyDocument = nullptr;
    document::Document* sheet = nullptr;
    for (const auto& d : other->documents().project().documents()) {
        if (d->name == "Опора") support = d.get();
        if (d->kind() == DocumentKind::Assembly) assemblyDocument = d.get();
        if (d->name == "Опора. Чертёж") sheet = d.get();
    }
    ASSERT_NE(support, nullptr);
    ASSERT_NE(assemblyDocument, nullptr);
    ASSERT_NE(sheet, nullptr);
    // Тело в файле не хранится, но строится заново точно таким же.
    EXPECT_DOUBLE_EQ(other->modeling().mass(*support).value().geometry.volume, volume);
    const auto* pin = assemblyDocument->objectsOf<assembly::Component>().at(1);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(pin->placement.m[i], pinPlace.m[i]);
    EXPECT_TRUE(other->assembly().solve(*assemblyDocument).value().solved);
    EXPECT_EQ(sheet->objectsOf<drawing::ModelView>().size(), 3u);
    EXPECT_EQ(other->updateModelViews(*sheet).value(), 3);
    fs::remove_all(path);
}

TEST_F(Demo, StepExportOfAssemblyContainsBothBodies) {
    auto bodies = context->bodiesOf(*demo.assembly);
    ASSERT_TRUE(bodies.ok());
    ASSERT_EQ(bodies.value().size(), 2u);
    const fs::path path = fs::temp_directory_path() / "cosmocad_demo_assembly.step";
    ASSERT_TRUE(context->kernel().exportStep(bodies.value(), path).ok());
    auto back = context->kernel().importStep(path, "imp");
    ASSERT_TRUE(back.ok());
    const double total = context->kernel().massProperties(bodies.value()[0]).value().volume +
                         context->kernel().massProperties(bodies.value()[1]).value().volume;
    EXPECT_NEAR(context->kernel().massProperties(back.value()).value().volume, total, 1e-2);
    EXPECT_FALSE(context->bodiesOf(*demo.drawing).ok());
}

TEST(ContourFromDrawing, FindsLoopsCirclesAndHoles) {
    auto context = app::AppContext::create(resources()).take();
    auto created = context->documents().createDocument(DocumentKind::Drawing, "Эскиз");
    document::Document& doc = *created.value();
    drawing::IDrawingService& service = context->drawing();
    auto rectangle = service.addRectangle(doc, {10, 10}, {70, 50});
    auto hole = service.addCircle(doc, {40, 30}, 8);
    auto text = service.addText(doc, {0, 0}, "не контур", 3.5);
    std::vector<core::Uuid> ids = rectangle.value();
    ids.push_back(hole.value());
    ids.push_back(text.value());
    auto shapes = app::AppContext::contourFromDrawing(doc, ids);
    ASSERT_TRUE(shapes.ok()) << shapes.error().message;
    ASSERT_EQ(shapes.value().size(), 2u);
    EXPECT_EQ(shapes.value()[0].kind, modeling::SketchShape::Kind::Polygon);
    EXPECT_FALSE(shapes.value()[0].hole);
    EXPECT_EQ(shapes.value()[0].values.size(), 8u);
    EXPECT_EQ(shapes.value()[1].kind, modeling::SketchShape::Kind::Circle);
    EXPECT_TRUE(shapes.value()[1].hole);

    // Из контура получается тело нужного объёма.
    auto part = context->documents().createDocument(DocumentKind::Part, "Из чертежа");
    auto sketch = context->modeling().addSketch(*part.value(), {"", "XY", "0", "", shapes.value()});
    ASSERT_TRUE(sketch.ok()) << sketch.error().message;
    ASSERT_TRUE(context->modeling().addExtrude(*part.value(), {sketch.value(), "5", false, false}).ok());
    ASSERT_TRUE(context->modeling().recompute(*part.value()).ok());
    EXPECT_NEAR(context->modeling().mass(*part.value()).value().geometry.volume,
                (60.0 * 40 - std::numbers::pi * 64) * 5, 1e-4);

    // Незамкнутый контур и пустой выбор — понятные ошибки.
    auto open = rectangle.value();
    open.pop_back();
    EXPECT_FALSE(app::AppContext::contourFromDrawing(doc, open).ok());
    EXPECT_FALSE(app::AppContext::contourFromDrawing(doc, {text.value()}).ok());
}

// Разрез на чертеже (требование ЧЕР-5): срез заштрихован, над разрезом надпись,
// на виде сверху — обозначение секущей плоскости.
TEST_F(Demo, SectionViewIsHatchedLabelledAndMarked) {
    const auto count = [&](const char* name) {
        int n = 0;
        for (const auto* e : demo.drawing->objectsOf<drawing::Entity>()) n += e->name == name ? 1 : 0;
        return n;
    };
    const std::size_t before = demo.drawing->objectsOf<drawing::Entity>().size();
    auto added = context->addSectionView(*demo.drawing, *demo.support, "front", {150, 120}, 1.0);
    ASSERT_TRUE(added.ok()) << added.error().message;
    const auto* section = demo.drawing->findAs<drawing::ModelView>(added.value());
    ASSERT_NE(section, nullptr);
    EXPECT_TRUE(section->section);
    EXPECT_EQ(section->label, "А–А");
    EXPECT_NEAR(section->sectionAt, 25.0, 1e-6);  // середина плиты шириной 50

    // Плоскость идёт через центральное отверстие: материал слева и справа от него.
    const auto hatches = demo.drawing->objectsOf<drawing::Hatch>();
    ASSERT_EQ(hatches.size(), 2u);
    double hatched = 0.0;
    for (const auto* hatch : hatches) {
        ASSERT_EQ(hatch->loops.size(), 1u);
        EXPECT_FALSE(hatch->strokes().empty());
        double area = 0.0;
        const auto& loop = hatch->loops[0];
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const auto a = loop[i], b = loop[(i + 1) % loop.size()];
            area += (a.x * b.y - b.x * a.y) / 2.0;
        }
        hatched += std::abs(area);
    }
    // Плита 80×10 и бобышка 36×15 без отверстия 24×25; скругление и фаска меняют площадь немного.
    EXPECT_NEAR(hatched, 80.0 * 10 + 36.0 * 15 - 24.0 * 25, 12.0);
    EXPECT_EQ(count("Обозначение разреза"), 1);
    EXPECT_EQ(count("Секущая плоскость"), 2);
    EXPECT_EQ(count("Буква разреза"), 2);
    EXPECT_EQ(section->marks.size(), 10u);
    // Штрихи стоят на виде сверху (середина вида y = 170) по обе стороны от плиты длиной 80.
    for (const auto* e : demo.drawing->objectsOf<drawing::LineEntity>())
        if (e->name == "Секущая плоскость") {
            EXPECT_NEAR(e->start.y, 170.0, 1e-6);
            EXPECT_NEAR(e->end.y, 170.0, 1e-6);
            EXPECT_GT(std::abs(e->start.x - 85.0), 40.0);
        }

    // Модель меняется — разрез и обозначение обновляются, ничего не двоится.
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "100").ok());
    auto updated = context->updateModelViews(*demo.drawing);
    ASSERT_TRUE(updated.ok()) << updated.error().message;
    EXPECT_EQ(updated.value(), 4);
    EXPECT_EQ(demo.drawing->objectsOf<drawing::Hatch>().size(), 2u);
    EXPECT_EQ(count("Обозначение разреза"), 1);
    EXPECT_EQ(count("Секущая плоскость"), 2);
    EXPECT_EQ(demo.drawing->findAs<drawing::ModelView>(added.value())->marks.size(), 10u);

    // Второй разрез получает следующую букву; плоскость мимо тела — отказ.
    auto second = context->addSectionView(*demo.drawing, *demo.support, "left", {60, 60}, 1.0, 10.0);
    ASSERT_TRUE(second.ok()) << second.error().message;
    EXPECT_EQ(demo.drawing->findAs<drawing::ModelView>(second.value())->label, "Б–Б");
    EXPECT_FALSE(context->addSectionView(*demo.drawing, *demo.support, "left", {60, 60}, 1.0, 500.0).ok());

    // Отмена: второй разрез, обновление четырёх видов, первый разрез — каждое одним шагом.
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(demo.drawing->undo());
    EXPECT_EQ(demo.drawing->objectsOf<drawing::Entity>().size(), before);
    EXPECT_TRUE(demo.drawing->objectsOf<drawing::Hatch>().empty());
}

// В разрезе сборки соседние детали штрихуются в разные стороны (ГОСТ 2.306-68).
TEST_F(Demo, AssemblySectionHatchesNeighboursDifferently) {
    auto added = context->addSectionView(*demo.drawing, *demo.assembly, "front", {150, 120}, 1.0);
    ASSERT_TRUE(added.ok()) << added.error().message;
    std::set<long> angles;
    for (const auto* hatch : demo.drawing->objectsOf<drawing::Hatch>()) angles.insert(std::lround(hatch->angle * 1000));
    EXPECT_EQ(angles.size(), 2u);
    EXPECT_EQ(demo.drawing->objectsOf<drawing::Hatch>().size(), 3u);  // опора слева и справа от пальца и сам палец
}

// Сопряжения «касание» и «угол» (требование СБ-2).
TEST_F(Demo, TangentAndAngleMatesPlaceComponent) {
    auto& service = context->assembly();
    // Второй палец лежит где-то в стороне, повёрнутый как попало.
    const Placement start = Placement::translation(150, 60, 40) * Placement::rotation({0, 0, 0}, {1, 1, 0}, 0.7);
    auto roller = service.addComponent(*demo.assembly, demo.pin->id(), start);
    ASSERT_TRUE(roller.ok());
    const core::Uuid supportId = component(0)->id();

    // Касание: цилиндр пальца ложится на верх плиты.
    auto tangent = service.addMate(*demo.assembly, {assembly::MateKind::Tangent, {supportId, "E1/end"}, {roller.value(), "E1/side/1000"}});
    ASSERT_TRUE(tangent.ok()) << tangent.error().message;
    auto solved = service.solve(*demo.assembly);
    ASSERT_TRUE(solved.ok());
    EXPECT_TRUE(solved.value().solved);
    const auto* placed = demo.assembly->findAs<assembly::Component>(roller.value());
    const auto axis = placed->placement.rotate({0, 0, 1});
    EXPECT_NEAR(axis.z, 0.0, 1e-9);  // ось пальца параллельна плите
    EXPECT_NEAR(placed->placement.apply({0, 0, 0}).z, 10.0 + 11.95, 1e-6);  // ось — на радиус выше плиты толщиной 10
    // Касание выполняется относительно плоскости грани: вдоль плиты палец остаётся там, где был.

    // Неподходящие грани отклоняются.
    EXPECT_FALSE(service.addMate(*demo.assembly, {assembly::MateKind::Tangent, {supportId, "E1/end"}, {roller.value(), "E1/end"}}).ok());
    EXPECT_FALSE(service.addMate(*demo.assembly, {assembly::MateKind::Angle, {supportId, "E1/end"}, {roller.value(), "E1/side/1000"}}).ok());
    EXPECT_FALSE(service.addMate(*demo.assembly, {assembly::MateKind::Angle, {supportId, "E1/end"}, {roller.value(), "E1/end"}, 200.0}).ok());

    // Угол: торец пальца под 60° к верху плиты. Вместе с касанием это выполнить нельзя
    // (касание требует 90°) — программа сообщает о противоречии.
    auto angle = service.addMate(*demo.assembly, {assembly::MateKind::Angle, {supportId, "E1/end"}, {roller.value(), "E1/end"}, 60.0});
    ASSERT_TRUE(angle.ok()) << angle.error().message;
    solved = service.solve(*demo.assembly);
    ASSERT_TRUE(solved.ok());
    EXPECT_FALSE(solved.value().solved);
    // Без касания угол выполняется.
    ASSERT_TRUE(service.removeMate(*demo.assembly, tangent.value()).ok());
    solved = service.solve(*demo.assembly);
    ASSERT_TRUE(solved.ok());
    EXPECT_TRUE(solved.value().solved);
    const auto end = demo.assembly->findAs<assembly::Component>(roller.value())->placement.rotate({0, 0, 1});
    EXPECT_NEAR(end.z, 0.5, 1e-6);  // cos 60°: торец «E1/end» смотрит вдоль оси пальца
    // Угол 90° совместим с касанием.
    ASSERT_TRUE(service.removeMate(*demo.assembly, angle.value()).ok());
    ASSERT_TRUE(service.addMate(*demo.assembly, {assembly::MateKind::Tangent, {roller.value(), "E1/side/1000"}, {supportId, "E1/end"}}).ok());
    ASSERT_TRUE(service.addMate(*demo.assembly, {assembly::MateKind::Angle, {supportId, "E1/end"}, {roller.value(), "E1/end"}, 90.0}).ok());
    solved = service.solve(*demo.assembly);
    ASSERT_TRUE(solved.ok());
    EXPECT_TRUE(solved.value().solved);
}

// Палец в разрезе сборки показывается нерассечённым, если так помечен (ГОСТ 2.305-2008).
TEST_F(Demo, MarkedComponentStaysWholeInSection) {
    auto added = context->addSectionView(*demo.drawing, *demo.assembly, "front", {150, 120}, 1.0);
    ASSERT_TRUE(added.ok()) << added.error().message;
    ASSERT_EQ(demo.drawing->objectsOf<drawing::Hatch>().size(), 3u);
    const std::size_t linesBefore = demo.drawing->findAs<drawing::ModelView>(added.value())->entities.size();

    ASSERT_TRUE(context->assembly().setWholeInSection(*demo.assembly, component(1)->id(), true).ok());
    ASSERT_TRUE(context->updateModelViews(*demo.drawing).ok());
    // Штрихуется только опора; палец виден целым — линий на разрезе стало больше.
    EXPECT_EQ(demo.drawing->objectsOf<drawing::Hatch>().size(), 2u);
    EXPECT_NE(demo.drawing->findAs<drawing::ModelView>(added.value())->entities.size(), linesBefore);

    // Пометка хранится в файле и отменяется.
    EXPECT_TRUE(component(1)->wholeInSection);
    ASSERT_TRUE(demo.assembly->undo());
    EXPECT_FALSE(component(1)->wholeInSection);
    // Если целыми помечены все компоненты, рассекать нечего — понятный отказ.
    ASSERT_TRUE(context->assembly().setWholeInSection(*demo.assembly, component(0)->id(), true).ok());
    ASSERT_TRUE(context->assembly().setWholeInSection(*demo.assembly, component(1)->id(), true).ok());
    EXPECT_FALSE(context->addSectionView(*demo.drawing, *demo.assembly, "left", {60, 60}, 1.0).ok());
}

// Степени свободы компонентов (требование СБ-3).
TEST_F(Demo, FreedomIsCountedPerComponent) {
    auto& service = context->assembly();
    const auto freeOf = [&](const core::Uuid& id) {
        auto all = service.degreesOfFreedom(*demo.assembly);
        EXPECT_TRUE(all.ok());
        for (const auto& item : all.value())
            if (item.componentId == id) return item.fixed ? -1 : item.free;
        return -2;
    };
    // Опора закреплена; палец на оси отверстия и упёрт торцом — остался только поворот вокруг оси.
    EXPECT_EQ(freeOf(component(0)->id()), -1);
    EXPECT_EQ(freeOf(component(1)->id()), 1);

    // Новый компонент ничем не удерживается.
    auto roller = service.addComponent(*demo.assembly, demo.pin->id(), Placement::translation(150, 60, 40));
    ASSERT_TRUE(roller.ok());
    EXPECT_EQ(freeOf(roller.value()), 6);
    // Касание плоскости отнимает два движения: подъём над плоскостью и наклон оси.
    ASSERT_TRUE(service.addMate(*demo.assembly, {assembly::MateKind::Tangent, {component(0)->id(), "E1/end"}, {roller.value(), "E1/side/1000"}}).ok());
    ASSERT_TRUE(service.solve(*demo.assembly).ok());
    EXPECT_EQ(freeOf(roller.value()), 4);
    EXPECT_EQ(freeOf(component(1)->id()), 1);  // на первый палец это не повлияло

    // Без упора торцом палец ещё и скользит вдоль оси.
    for (const auto* mate : demo.assembly->objectsOf<assembly::Mate>())
        if (mate->kind == assembly::MateKind::Coincident) {
            ASSERT_TRUE(service.removeMate(*demo.assembly, mate->id()).ok());
            break;
        }
    EXPECT_EQ(freeOf(component(1)->id()), 2);
}

// Сечение показывает только срез; секущую плоскость готового разреза можно перенести.
TEST_F(Demo, CutOnlyViewAndMovingSectionPlane) {
    auto section = context->addSectionView(*demo.drawing, *demo.support, "front", {150, 120}, 1.0);
    ASSERT_TRUE(section.ok()) << section.error().message;
    const std::size_t fullLines = demo.drawing->findAs<drawing::ModelView>(section.value())->entities.size();
    auto cut = context->addSectionView(*demo.drawing, *demo.support, "front", {150, 80}, 1.0, std::nullopt, true);
    ASSERT_TRUE(cut.ok()) << cut.error().message;
    const auto* cutView = demo.drawing->findAs<drawing::ModelView>(cut.value());
    EXPECT_TRUE(cutView->sectionOnly);
    EXPECT_EQ(cutView->label, "Б–Б");
    EXPECT_LT(cutView->entities.size(), fullLines);  // за плоскостью ничего не показано
    EXPECT_EQ(demo.drawing->objectsOf<drawing::Hatch>().size(), 4u);

    // Плоскость А–А переносится с середины (25) на 20: обозначение на виде сверху сдвигается на 5 мм вниз.
    const auto markY = [&] {
        for (const auto& id : demo.drawing->findAs<drawing::ModelView>(section.value())->marks)
            if (const auto* line = demo.drawing->findAs<drawing::LineEntity>(id); line && line->name == "Секущая плоскость")
                return line->start.y;
        return 0.0;
    };
    EXPECT_NEAR(markY(), 170.0, 1e-6);
    ASSERT_TRUE(context->moveSection(*demo.drawing, section.value(), 20.0).ok());
    EXPECT_NEAR(demo.drawing->findAs<drawing::ModelView>(section.value())->sectionAt, 20.0, 1e-9);
    EXPECT_NEAR(markY(), 165.0, 1e-6);
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);
    // Плоскость мимо детали — отказ, разрез остаётся прежним; перенос отменяется одним шагом.
    EXPECT_FALSE(context->moveSection(*demo.drawing, section.value(), 500.0).ok());
    EXPECT_NEAR(demo.drawing->findAs<drawing::ModelView>(section.value())->sectionAt, 20.0, 1e-9);
    ASSERT_TRUE(demo.drawing->undo());
    EXPECT_NEAR(demo.drawing->findAs<drawing::ModelView>(section.value())->sectionAt, 25.0, 1e-9);
    EXPECT_NEAR(markY(), 170.0, 1e-6);
}

// Пометка «вид устарел» (требование ЧЕР-6): модель изменилась, а вид ещё не обновлён.
TEST_F(Demo, StaleViewsAreCountedUntilUpdated) {
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "90").ok());
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 2);  // на виде слева длина не видна — он не устарел
    // Длина возвращена — виды снова соответствуют модели.
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "80").ok());
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);
    ASSERT_TRUE(context->modeling().setParameter(*demo.support, "длина", "90").ok());
    ASSERT_TRUE(context->updateModelViews(*demo.drawing).ok());
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);

    // После сохранения и открытия файл не считается устаревшим.
    const fs::path path = fs::temp_directory_path() / "cosmocad_stale.cosmocad";
    fs::remove_all(path);
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto other = app::AppContext::create(resources()).take();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    for (const auto& document : other->documents().project().documents())
        if (document->kind() == DocumentKind::Drawing && !document->objectsOf<drawing::ModelView>().empty()) {
            EXPECT_EQ(other->staleViewCount(*document).value(), 0);
        }
    fs::remove_all(path);
}

// Общий знак шероховатости в правом верхнем углу листа (ГОСТ 2.309-73, пп. 2.6, 2.7).
TEST_F(Demo, SheetRoughnessSignSitsInCornerAndPassesCheck) {
    using Kind = drawing::RoughnessSign::Kind;
    ASSERT_TRUE(context->setSheetRoughness(*demo.drawing, "Ra 6,3", Kind::Any, true).ok());
    auto signs = demo.drawing->objectsOf<drawing::RoughnessSign>();
    ASSERT_EQ(signs.size(), 1u);
    EXPECT_TRUE(signs[0]->others);
    EXPECT_NEAR(signs[0]->textHeight / signs[0]->othersHeight, 1.5, 0.1);  // общий знак крупнее примерно в 1,5 раза
    const auto box = signs[0]->bounds();
    // Лист А4 стоя: рамка до x = 205, y = 292.
    EXPECT_NEAR(box.max.x, 205.0 - 7.5, 1e-6);
    EXPECT_NEAR(box.max.y, 292.0 - 7.5, 1e-6);
    EXPECT_TRUE(context->standards().check(*demo.drawing).value().empty());

    // Повторная команда заменяет знак, а не добавляет второй; неверная запись отклоняется.
    ASSERT_TRUE(context->setSheetRoughness(*demo.drawing, "Rz 40", Kind::Removal, false).ok());
    signs = demo.drawing->objectsOf<drawing::RoughnessSign>();
    ASSERT_EQ(signs.size(), 1u);
    EXPECT_EQ(signs[0]->parameter, "Rz 40");
    EXPECT_FALSE(context->setSheetRoughness(*demo.drawing, "гладко", Kind::Any, false).ok());
    EXPECT_EQ(demo.drawing->objectsOf<drawing::RoughnessSign>().front()->parameter, "Rz 40");
    // Пустое значение убирает знак; отмена возвращает.
    ASSERT_TRUE(context->setSheetRoughness(*demo.drawing, "", Kind::Any, false).ok());
    EXPECT_TRUE(demo.drawing->objectsOf<drawing::RoughnessSign>().empty());
    ASSERT_TRUE(demo.drawing->undo());
    EXPECT_EQ(demo.drawing->objectsOf<drawing::RoughnessSign>().size(), 1u);
}

// Разнесённый вид сборки (требование СБ-7).
TEST_F(Demo, ExplodedViewMovesComponentsWithoutTouchingMates) {
    auto& service = context->assembly();
    const Placement before = component(1)->placement;
    auto moved = service.autoExplode(*demo.assembly, 50.0);
    ASSERT_TRUE(moved.ok()) << moved.error().message;
    EXPECT_EQ(moved.value(), 1);  // закреплённая опора остаётся на месте
    // Палец стоит на оси отверстия: его середина выше середины опоры — он уезжает вверх.
    EXPECT_NEAR(component(1)->explode.z, 50.0, 1e-9);
    EXPECT_NEAR(component(1)->explode.x, 0.0, 1e-9);
    EXPECT_EQ(component(1)->placement.m, before.m);  // место по сопряжениям не изменилось

    const auto top = [&](bool exploded) {
        auto bodies = context->bodiesOf(*demo.assembly, exploded);
        EXPECT_TRUE(bodies.ok());
        return context->kernel().bounds(bodies.value()[1]).value().max.z;
    };
    EXPECT_NEAR(top(true) - top(false), 50.0, 1e-6);
    // Собранный вид, зазоры и пересечения считаются по собранному состоянию.
    EXPECT_NEAR(service.clearance(*demo.assembly, component(0)->id(), component(1)->id()).value(), 0.05, 1e-6);
    ASSERT_TRUE(service.solve(*demo.assembly).ok());
    EXPECT_NEAR(component(1)->explode.z, 50.0, 1e-9);

    // Разнесённый вид ставится на чертёж и обновляется как обычный.
    auto view = context->addModelView(*demo.drawing, *demo.assembly, "front", {150, 110}, 1.0, false, true);
    ASSERT_TRUE(view.ok()) << view.error().message;
    EXPECT_TRUE(demo.drawing->findAs<drawing::ModelView>(view.value())->exploded);
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);
    ASSERT_TRUE(service.setExplode(*demo.assembly, component(1)->id(), {0, 0, 80}).ok());
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 1);  // устарел только разнесённый вид
    ASSERT_TRUE(context->updateModelViews(*demo.drawing).ok());
    EXPECT_EQ(context->staleViewCount(*demo.drawing).value(), 0);

    // Разнос убирается; отрицательное расстояние — отказ.
    EXPECT_FALSE(service.autoExplode(*demo.assembly, -1.0).ok());
    ASSERT_TRUE(service.autoExplode(*demo.assembly, 0.0).ok());
    EXPECT_NEAR(component(1)->explode.z, 0.0, 1e-12);
    EXPECT_EQ(service.autoExplode(*demo.assembly, 0.0).value(), 0);
}

// Технические требования над основной надписью (ГОСТ 2.316-2008).
TEST_F(Demo, TechnicalRequirementsAreNumberedAboveTitleBlock) {
    const std::vector<std::string> items = {
        "Неуказанные предельные отклонения размеров: отверстий — по H14, валов — по h14, остальных — ±IT14/2.",
        "Острые кромки притупить.",
        "Покрытие — по согласованию с заказчиком; допускается поставка без покрытия при условии консервации изделия "
        "на весь срок хранения и транспортирования, но не менее чем на двенадцать месяцев со дня отгрузки."};
    ASSERT_TRUE(context->setTechnicalRequirements(*demo.drawing, items).ok());
    std::vector<const drawing::TextEntity*> lines;
    for (const auto* text : demo.drawing->objectsOf<drawing::TextEntity>())
        if (text->name.rfind("Технические требования", 0) == 0) lines.push_back(text);
    ASSERT_GT(lines.size(), 3u);  // длинный пункт перенесён на несколько строк
    int numbered = 0;
    for (const auto* line : lines) {
        // Колонка стоит над основной надписью (x от 20 до 205), не шире 185 мм, и выше надписи (y > 60).
        EXPECT_GE(line->position.x, 20.0 - 1e-9);
        EXPECT_LE(line->position.x + static_cast<double>(drawing::characterCount(line->text)) * 3.5 * 0.62, 205.0 + 1e-6);
        EXPECT_GT(line->position.y, 60.0);
        if (line->name == "Технические требования") ++numbered;
        EXPECT_NE(line->text, "Технические требования");  // заголовок не пишется
    }
    EXPECT_EQ(numbered, 3);
    // Пункты читаются обратно целиком; оформление листа остаётся без замечаний.
    EXPECT_EQ(context->technicalRequirements(*demo.drawing), items);
    EXPECT_TRUE(context->standards().check(*demo.drawing).value().empty());

    // Замена, а не добавление; пустой пункт — отказ; пустой список убирает требования; отмена возвращает.
    ASSERT_TRUE(context->setTechnicalRequirements(*demo.drawing, {"Острые кромки притупить."}).ok());
    EXPECT_EQ(context->technicalRequirements(*demo.drawing).size(), 1u);
    EXPECT_FALSE(context->setTechnicalRequirements(*demo.drawing, {"  "}).ok());
    ASSERT_TRUE(context->setTechnicalRequirements(*demo.drawing, {}).ok());
    EXPECT_TRUE(context->technicalRequirements(*demo.drawing).empty());
    ASSERT_TRUE(demo.drawing->undo());
    EXPECT_EQ(context->technicalRequirements(*demo.drawing).size(), 1u);
    // Слишком много пунктов не помещается над основной надписью — понятный отказ.
    EXPECT_FALSE(context->setTechnicalRequirements(*demo.drawing, std::vector<std::string>(60, "Пункт.")).ok());
}
