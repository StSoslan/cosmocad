// Электрические схемы (требования СХ-1…СХ-6).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <set>
#include <filesystem>
#include <map>
#include <fstream>
#include <numbers>
#include <sstream>

#include "AppContext.h"
#include "cosmocad/graphics/Scene.h"

using namespace cosmocad;
namespace fs = std::filesystem;
using geometry::Point2;
using schematic::Issue;

namespace {

fs::path resources() { return COSMOCAD_TEST_RESOURCES; }

struct Scheme : ::testing::Test {
    std::unique_ptr<app::AppContext> context = app::AppContext::create(resources()).take();
    document::Document* doc = nullptr;

    void SetUp() override {
        auto created = context->createDemoSchematic();
        ASSERT_TRUE(created.ok()) << created.error().message;
        doc = created.value();
    }
    schematic::SchematicService& s() { return context->schematic(); }
    const schematic::Element* element(const std::string& designator) {
        for (const auto* e : doc->objectsOf<schematic::Element>())
            if (e->designator == designator) return e;
        return nullptr;
    }
    std::string pinsOf(const schematic::Net& net) {
        std::string out;
        for (const auto& pin : net.pins) out += pin.designator + "." + pin.pin + " ";
        return out;
    }
    bool has(const std::vector<Issue>& issues, Issue::Kind kind) {
        return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) { return i.kind == kind; });
    }
};

}  // namespace

TEST(SymbolLibrary, LoadsVerifiedSymbolsAndRejectsBadFiles) {
    auto library = schematic::SymbolLibrary::load(resources() / "standards" / "eskd" / "symbols.json");
    ASSERT_TRUE(library.ok()) << library.error().message;
    ASSERT_NE(library.value().find("resistor"), nullptr);
    ASSERT_NE(library.value().find("capacitor"), nullptr);
    EXPECT_EQ(library.value().find("resistor")->prefix, "R");
    EXPECT_EQ(library.value().find("resistor")->pins.size(), 2u);
    // Резистор — прямоугольник 10×4 мм: среди отрезков есть стороны длиной 10 и 4.
    int ten = 0, four = 0;
    for (const auto& g : library.value().find("resistor")->graphics) {
        const double length = geometry::distance(g.a, g.b);
        ten += std::abs(length - 10.0) < 1e-9 ? 1 : 0;
        four += std::abs(length - 4.0) < 1e-9 ? 1 : 0;
    }
    EXPECT_EQ(ten, 2);
    EXPECT_EQ(four, 2);
    // У каждого обозначения записано, по какому стандарту оно сделано и чем подтверждено.
    for (const auto& symbol : library.value().symbols) {
        EXPECT_FALSE(symbol.standard.empty()) << symbol.id;
        EXPECT_FALSE(symbol.source.empty()) << symbol.id;
    }
    // Набор составлен по стандартам, присланным владельцем проекта.
    EXPECT_EQ(library.value().symbols.size(), 20u);
    // Катушка: цепочка полуокружностей без разрывов от вывода до вывода.
    const auto* coil = library.value().find("inductor");
    ASSERT_NE(coil, nullptr);
    EXPECT_EQ(coil->prefix, "L");
    int arcs = 0;
    double covered = 0.0;
    for (const auto& g : coil->graphics) {
        if (g.kind == drawing::Primitive::Kind::Arc) {
            ++arcs;
            covered += 2.0 * g.radius;
            EXPECT_NEAR(g.endAngle - g.startAngle, std::numbers::pi, 1e-9);
        } else {
            covered += geometry::distance(g.a, g.b);
        }
    }
    EXPECT_EQ(arcs, 4);
    EXPECT_NEAR(covered, 20.0, 1e-9);
    // Электролитический конденсатор: обкладка-прямоугольник 1,5×8 мм и зазор 1,5 мм (ГОСТ 2.728-74).
    const auto* polar = library.value().find("capacitor_polar");
    ASSERT_NE(polar, nullptr);
    EXPECT_NEAR(polar->graphics[1].b.x - polar->graphics[1].a.x, 1.5, 1e-9);  // нижняя сторона прямоугольника
    EXPECT_NEAR(polar->graphics[5].a.x - polar->graphics[2].a.x, 1.5, 1e-9);  // от прямоугольника до черты
    ASSERT_NE(library.value().find("resistor_var"), nullptr);
    // Транзистор: три вывода на шаге 5 мм, буква VT (ГОСТ 2.710-81).
    for (const char* id : {"transistor_pnp", "transistor_npn"}) {
        const auto* transistor = library.value().find(id);
        ASSERT_NE(transistor, nullptr) << id;
        EXPECT_EQ(transistor->prefix, "VT");
        ASSERT_EQ(transistor->pins.size(), 3u);
        for (const auto& pin : transistor->pins) {
            EXPECT_NEAR(std::fmod(std::abs(pin.at.x), 5.0), 0.0, 1e-9);
            EXPECT_NEAR(std::fmod(std::abs(pin.at.y), 5.0), 0.0, 1e-9);
        }
    }
    ASSERT_NE(library.value().find("contact_nc"), nullptr);
    // Замыкающий контакт: подвижная часть наклонена на 30°.
    const auto& blade = library.value().find("contact_no")->graphics[1];
    EXPECT_NEAR(std::atan2(blade.b.y - blade.a.y, blade.b.x - blade.a.x), std::numbers::pi / 6, 1e-3);
    // Лампа: круг ⌀10 мм с косым крестом, концы креста лежат на окружности.
    const auto* lamp = library.value().find("lamp");
    ASSERT_NE(lamp, nullptr);
    EXPECT_EQ(lamp->prefix, "EL");
    EXPECT_EQ(library.value().find("signal_lamp")->prefix, "HL");
    int onCircle = 0;
    for (const auto& g : lamp->graphics)
        if (g.kind == drawing::Primitive::Kind::Line && std::abs(geometry::distance(g.a, {0, 0}) - 5.0) < 1e-6 &&
            std::abs(geometry::distance(g.b, {0, 0}) - 5.0) < 1e-6)
            ++onCircle;
    EXPECT_EQ(onCircle, 2);
    // Буквы обозначений — по ГОСТ 2.710-81.
    for (const auto& [id, prefix] : {std::pair{"resistor", "R"}, std::pair{"capacitor", "C"}, std::pair{"fuse", "FU"},
                                     std::pair{"diode", "VD"}, std::pair{"contact_no", "SA"}, std::pair{"cell", "GB"},
                                     std::pair{"ammeter", "PA"}, std::pair{"voltmeter", "PV"}}) {
        ASSERT_NE(library.value().find(id), nullptr) << id;
        EXPECT_EQ(library.value().find(id)->prefix, prefix) << id;
    }
    // Амперметр: окружность ⌀10 мм с буквой внутри (ГОСТ 2.747-68, 2.729-68).
    bool circle = false, letter = false;
    for (const auto& g : library.value().find("ammeter")->graphics) {
        circle = circle || (g.kind == drawing::Primitive::Kind::Circle && std::abs(g.radius - 5.0) < 1e-9);
        letter = letter || (g.kind == drawing::Primitive::Kind::Text && g.text == "A");
    }
    EXPECT_TRUE(circle);
    EXPECT_TRUE(letter);
    // Заземление: один вывод, черты 5, 3 и 1 шаг (ГОСТ 2.721-74, приложение 2).
    const auto* ground = library.value().find("ground");
    ASSERT_NE(ground, nullptr);
    EXPECT_EQ(ground->pins.size(), 1u);
    const auto* fuse = library.value().find("fuse");
    ASSERT_NE(fuse, nullptr);
    EXPECT_EQ(fuse->prefix, "FU");
    // Диод: треугольник 4×4 шага сетки и черта катода 4 шага (ГОСТ 2.730-73, приложение 2).
    const auto* diode = library.value().find("diode");
    ASSERT_NE(diode, nullptr);
    geometry::BBox2 body;
    for (std::size_t i = 1; i < diode->graphics.size(); ++i) {  // первая линия — выводы
        body.expand(diode->graphics[i].a);
        body.expand(diode->graphics[i].b);
    }
    EXPECT_NEAR(body.width(), 4.0, 1e-9);
    EXPECT_NEAR(body.height(), 4.0, 1e-9);
    ASSERT_NE(library.value().find("contact_no"), nullptr);
    EXPECT_EQ(library.value().find("triac"), nullptr);  // неразобранных обозначений в наборе нет

    const fs::path dir = fs::temp_directory_path() / "cosmocad_symbols";
    fs::create_directories(dir);
    EXPECT_FALSE(schematic::SymbolLibrary::load(dir / "нет.json").ok());
    std::ofstream(dir / "bad.json") << "{ \"version\": \"1\", \"symbols\": [ { \"id\": \"x\" } ] }";
    EXPECT_FALSE(schematic::SymbolLibrary::load(dir / "bad.json").ok());
    std::ofstream(dir / "garbage.json") << "это не json";
    EXPECT_FALSE(schematic::SymbolLibrary::load(dir / "garbage.json").ok());
}

TEST_F(Scheme, DemoNetsAreFoundAndNamedByLabels) {
    const auto nets = s().nets(*doc);
    ASSERT_EQ(nets.size(), 4u);
    EXPECT_EQ(nets[0].number, 1);
    EXPECT_EQ(nets[0].name, "Общий");
    EXPECT_EQ(pinsOf(nets[0]), "C1.1 R2.1 VD1.1 ");
    EXPECT_EQ(nets[1].name, "Выход");
    EXPECT_EQ(pinsOf(nets[1]), "C1.2 R1.2 R2.2 VD1.2 ");
    EXPECT_EQ(nets[2].name, "Вход");
    EXPECT_EQ(pinsOf(nets[2]), "FU1.1 ");
    EXPECT_EQ(nets[3].name, "");  // цепь между предохранителем и резистором — без имени
    EXPECT_EQ(pinsOf(nets[3]), "FU1.2 R1.1 ");
    EXPECT_TRUE(s().check(*doc).empty());
}

TEST_F(Scheme, DemoSheetPassesStandardsCheckAndIsDrawn) {
    auto violations = context->standards().check(*doc);
    ASSERT_TRUE(violations.ok());
    for (const auto& v : violations.value()) ADD_FAILURE() << v.ruleId << ": " << v.message;
    auto profile = context->profileFor(*doc);
    const auto scene = graphics::buildScene(*doc, profile ? &*profile : nullptr);
    int designators = 0, dots = 0;
    for (const auto& item : scene.items) {
        if (item.kind == graphics::SceneItem::Kind::Text && (item.text == "R1" || item.text == "R2" || item.text == "C1")) ++designators;
        // Точка соединения — окружность, закрытая собственной толщиной линии.
        if (item.kind == graphics::SceneItem::Kind::Circle && std::abs(item.radius - 0.4) < 1e-9) ++dots;
    }
    EXPECT_EQ(designators, 4);  // R1, R2, C1 на схеме и C1 в таблице перечня
    EXPECT_EQ(dots, 6);  // три ответвления вверх, три вниз
    // Элемент выбирается щелчком по своему контуру, привязка ловит вывод.
    EXPECT_EQ(graphics::hitTest(*doc, {80, 202}, 0.5).value_or(core::Uuid()), element("R1")->id());
    auto snap = graphics::findSnap(*doc, {70.3, 200.2}, 1.0);
    ASSERT_TRUE(snap.has_value());
    EXPECT_NEAR(snap->point.x, 70.0, 1e-9);
}

TEST_F(Scheme, CrossingWiresAreNotConnectedButTeeIs) {
    // Проводник пересекает линию выхода и общий провод, но не заканчивается на них.
    ASSERT_TRUE(s().addWire(*doc, {{150, 150}, {150, 210}}).ok());
    auto nets = s().nets(*doc);
    EXPECT_EQ(nets.size(), 5u);  // отдельная цепь без выводов
    EXPECT_TRUE(nets.back().pins.empty());
    EXPECT_TRUE(has(s().check(*doc), Issue::Kind::DanglingWire));
    ASSERT_TRUE(doc->undo());
    // А проводник, чей конец лежит на линии выхода, с ней соединён и получает точку.
    auto tee = s().addWire(*doc, {{150, 200}, {150, 215}});
    ASSERT_TRUE(tee.ok());
    EXPECT_EQ(s().nets(*doc).size(), 4u);
    EXPECT_EQ(doc->findAs<schematic::Wire>(tee.value())->junctions.size(), 1u);
    const auto issues = s().check(*doc);
    ASSERT_EQ(issues.size(), 1u);  // второй конец висит
    EXPECT_EQ(issues[0].kind, Issue::Kind::DanglingWire);
    EXPECT_EQ(issues[0].objectId, tee.value());
    ASSERT_TRUE(s().addLabel(*doc, {150, 215}, "Выход").ok());
    EXPECT_TRUE(s().check(*doc).empty());
}

TEST_F(Scheme, CheckFindsUnconnectedPinsDuplicatesAndLabelConflicts) {
    auto lonely = s().addElement(*doc, "resistor", {80, 230}, 0, "1 кОм");
    ASSERT_TRUE(lonely.ok());
    EXPECT_EQ(doc->findAs<schematic::Element>(lonely.value())->designator, "R3");  // следующий свободный номер
    auto issues = s().check(*doc);
    EXPECT_EQ(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.kind == Issue::Kind::UnconnectedPin; }), 2);
    // Занятое обозначение вручную не поставить…
    EXPECT_FALSE(s().setDesignator(*doc, lonely.value(), "R1").ok());
    EXPECT_FALSE(s().setDesignator(*doc, lonely.value(), "").ok());
    // …но копирование обычным инструментом чертежа создаёт повтор — проверка его находит.
    drawing::IDrawingService& drawingService = context->drawing();
    ASSERT_TRUE(drawingService.copyEntities(*doc, {lonely.value()}, {40, 0}).ok());
    EXPECT_TRUE(has(s().check(*doc), Issue::Kind::DuplicateDesignator));
    auto renumbered = s().renumber(*doc);
    ASSERT_TRUE(renumbered.ok());
    EXPECT_GT(renumbered.value(), 0);
    EXPECT_FALSE(has(s().check(*doc), Issue::Kind::DuplicateDesignator));
    // Две разные метки на одной цепи.
    ASSERT_TRUE(s().addLabel(*doc, {100, 200}, "Питание").ok());
    EXPECT_TRUE(has(s().check(*doc), Issue::Kind::LabelConflict));
    EXPECT_FALSE(s().addElement(*doc, "triac", {0, 0}).ok());
    EXPECT_FALSE(s().addElement(*doc, "resistor", {0, 0}, 45).ok());
    EXPECT_FALSE(s().addWire(*doc, {{1, 1}}).ok());
    EXPECT_FALSE(s().addWire(*doc, {{1, 1}, {1, 1}}).ok());
}

TEST_F(Scheme, WiresFollowMovedElement) {
    const auto before = s().nets(*doc);
    const core::Uuid r1 = element("R1")->id();
    ASSERT_TRUE(s().moveElements(*doc, {r1}, {0, 15}).ok());
    EXPECT_NEAR(element("R1")->pinPosition(0).y, 215.0, 1e-9);
    // Состав цепей не изменился: проводники остались на выводах.
    const auto after = s().nets(*doc);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) EXPECT_EQ(pinsOf(after[i]), pinsOf(before[i]));
    EXPECT_TRUE(s().check(*doc).empty());
    // Одно действие — одна отмена.
    ASSERT_TRUE(doc->undo());
    EXPECT_NEAR(element("R1")->pinPosition(0).y, 200.0, 1e-9);
    EXPECT_TRUE(s().check(*doc).empty());
}

TEST_F(Scheme, RenumberGoesLeftToRightAndListGroupsEqualElements) {
    const auto rowFor = [&](const std::string& start) {
        for (const auto& row : s().elementList(*doc))
            if (row.designators.rfind(start, 0) == 0) return row;
        return schematic::ListRow{};
    };
    ASSERT_EQ(s().elementList(*doc).size(), 4u);  // C1, FU1, R1 и R2 одной строкой, VD1
    EXPECT_EQ(s().elementList(*doc)[0].designators, "C1");
    EXPECT_EQ(s().elementList(*doc)[0].name, "Конденсатор 0,1 мкФ");
    EXPECT_EQ(rowFor("R").designators, "R1, R2");
    EXPECT_EQ(rowFor("R").count, 2);
    EXPECT_EQ(rowFor("VD").name, "Диод");
    // Новый резистор левее всех: после перенумерации он становится R1.
    auto left = s().addElement(*doc, "resistor", {40, 230}, 0, "10 кОм");
    ASSERT_TRUE(left.ok());
    ASSERT_TRUE(s().renumber(*doc).ok());
    EXPECT_EQ(doc->findAs<schematic::Element>(left.value())->designator, "R1");
    EXPECT_EQ(s().renumber(*doc).value(), 0);  // повторная перенумерация ничего не меняет
    // Запись группы — как в примере ГОСТ 2.701-2008, п. 5.7: «С8 ... С12».
    EXPECT_EQ(rowFor("R").designators, "R1 ... R3");
    EXPECT_EQ(rowFor("R").count, 3);
    // Другой номинал — отдельная строка.
    ASSERT_TRUE(s().setValue(*doc, left.value(), "22 кОм").ok());
    EXPECT_EQ(s().elementList(*doc).size(), 5u);

    const fs::path file = fs::temp_directory_path() / "cosmocad_list.csv";
    ASSERT_TRUE(s().exportElementListCsv(*doc, file).ok());
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("\"R2, R3\";\"Резистор 10 кОм\";2;"), std::string::npos);
}

// Таблица перечня на листе: размеры граф и место — по ГОСТ 2.701-2008, п. 5.7.
TEST_F(Scheme, ElementListTableOnSheetFollowsStandard) {
    const auto mine = [&] {
        std::vector<const drawing::Entity*> out;
        for (const auto* e : doc->objectsOf<drawing::Entity>())
            if (e->name == "Перечень элементов") out.push_back(e);
        return out;
    };
    geometry::BBox2 box;
    std::vector<double> verticals;
    for (const auto* e : mine())
        if (const auto* line = dynamic_cast<const drawing::LineEntity*>(e)) {
            box.expand(line->start);
            box.expand(line->end);
            if (std::abs(line->start.x - line->end.x) < 1e-9) verticals.push_back(line->start.x);
        }
    std::sort(verticals.begin(), verticals.end());
    ASSERT_EQ(verticals.size(), 5u);
    EXPECT_NEAR(box.width(), 185.0, 1e-9);
    EXPECT_NEAR(verticals[1] - verticals[0], 20.0, 1e-9);
    EXPECT_NEAR(verticals[2] - verticals[1], 110.0, 1e-9);
    EXPECT_NEAR(verticals[3] - verticals[2], 10.0, 1e-9);
    EXPECT_NEAR(box.height(), 15.0 + 8.0 * 4, 1e-9);  // головка 15 мм и четыре строки по 8 мм
    // Над основной надписью с зазором 12 мм.
    auto profile = context->profileFor(*doc);
    const auto geometry = standards::sheetGeometry(*doc->objectsOf<drawing::Sheet>().front(), *profile);
    EXPECT_NEAR(box.min.y - geometry->titleBlock.max.y, 12.0, 1e-9);
    EXPECT_NEAR(box.max.x, geometry->titleBlock.max.x, 1e-9);

    // Повторная постановка заменяет таблицу, а не добавляет вторую; отменяется одним шагом.
    const std::size_t before = mine().size();
    ASSERT_TRUE(s().addElement(*doc, "contact_no", {55, 230}, 0, "").ok());
    auto placed = context->placeElementListTable(*doc);
    ASSERT_TRUE(placed.ok()) << placed.error().message;
    EXPECT_EQ(placed.value(), 5);
    EXPECT_GT(mine().size(), before);
    ASSERT_TRUE(doc->undo());
    EXPECT_EQ(mine().size(), before);
    // На листе без элементов таблицу ставить не из чего.
    auto empty = context->documents().createDocument(document::DocumentKind::Drawing, "Пусто");
    EXPECT_FALSE(context->placeElementListTable(*empty.value()).ok());
}

TEST_F(Scheme, RotationPlacesPinsAndDrawingToolsRotateElements) {
    const auto* c1 = element("C1");  // стоит вертикально
    EXPECT_NEAR(c1->pinPosition(0).x, 110.0, 1e-9);
    EXPECT_NEAR(c1->pinPosition(0).y, 170.0, 1e-9);
    EXPECT_NEAR(c1->pinPosition(1).y, 190.0, 1e-9);
    // Поворот обычным инструментом чертежа на 90° вокруг центра элемента.
    drawing::IDrawingService& drawingService = context->drawing();
    const core::Uuid r1 = element("R1")->id();
    ASSERT_TRUE(drawingService.transformEntities(*doc, {r1}, geometry::Transform2::rotation({80, 200}, std::numbers::pi / 2), "Поворот").ok());
    EXPECT_EQ(element("R1")->rotation, 90);
    EXPECT_NEAR(element("R1")->pinPosition(0).y, 190.0, 1e-9);
    EXPECT_TRUE(has(s().check(*doc), Issue::Kind::DanglingWire));  // проводники за обычным поворотом не идут
}

TEST_F(Scheme, InstrumentKeepsLetterUprightAndSurvivesSave) {
    auto meter = s().addElement(*doc, "voltmeter", {60, 240}, 90, "");
    ASSERT_TRUE(meter.ok());
    const auto* element = doc->findAs<schematic::Element>(meter.value());
    EXPECT_EQ(element->designator, "PV1");
    // При повороте буква остаётся в центре окружности.
    bool found = false;
    for (const auto& p : element->primitives())
        if (p.kind == drawing::Primitive::Kind::Text && p.text == "V") {
            found = true;
            EXPECT_NEAR(p.a.x, 60.0, 1e-9);
            EXPECT_NEAR(p.a.y + p.textHeight / 2.0, 240.0, 1e-9);
        }
    EXPECT_TRUE(found);
    // Заземление с одним выводом присоединяется к общему проводу.
    auto ground = s().addElement(*doc, "ground", {80, 155}, 0, "");
    ASSERT_TRUE(ground.ok());
    EXPECT_NEAR(doc->findAs<schematic::Element>(ground.value())->pinPosition(0).y, 160.0, 1e-9);
    bool onCommon = false;
    for (const auto& net : s().nets(*doc))
        for (const auto& pin : net.pins) onCommon = onCommon || (pin.designator == "XE1" && net.name == "Общий");
    EXPECT_TRUE(onCommon);
    // Запись и чтение сохраняют букву внутри обозначения.
    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    schematic::registerTypes(factory);
    auto loaded = document::Document::fromJson(doc->toJson(), factory);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    bool letter = false;
    for (const auto* e : loaded.value()->objectsOf<schematic::Element>())
        if (e->designator == "PV1")
            for (const auto& g : e->graphics) letter = letter || g.text == "V";
    EXPECT_TRUE(letter);
}

// Элемент с тремя выводами: каждый вывод попадает в свою цепь.
TEST_F(Scheme, TransistorPinsJoinThreeNets) {
    auto vt = s().addElement(*doc, "transistor_npn", {100, 240}, 0, "КТ315");
    ASSERT_TRUE(vt.ok());
    EXPECT_EQ(doc->findAs<schematic::Element>(vt.value())->designator, "VT1");
    ASSERT_TRUE(s().addWire(*doc, {{90, 240}, {80, 240}}).ok());    // база
    ASSERT_TRUE(s().addWire(*doc, {{110, 245}, {120, 245}}).ok());  // коллектор
    ASSERT_TRUE(s().addWire(*doc, {{110, 235}, {120, 235}}).ok());  // эмиттер
    ASSERT_TRUE(s().addLabel(*doc, {80, 240}, "Управление").ok());
    ASSERT_TRUE(s().addLabel(*doc, {120, 245}, "Нагрузка").ok());
    ASSERT_TRUE(s().addLabel(*doc, {120, 235}, "Общий").ok());
    std::map<std::string, std::string> netOfPin;
    for (const auto& net : s().nets(*doc))
        for (const auto& pin : net.pins)
            if (pin.designator == "VT1") netOfPin[pin.pin] = net.name;
    EXPECT_EQ(netOfPin["Б"], "Управление");
    EXPECT_EQ(netOfPin["К"], "Нагрузка");
    EXPECT_EQ(netOfPin["Э"], "Общий");
    EXPECT_TRUE(s().check(*doc).empty());
}

TEST_F(Scheme, SchemeSurvivesSaveAndLoad) {
    const fs::path path = fs::temp_directory_path() / "cosmocad_scheme.cosmocad";
    fs::remove_all(path);
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto other = app::AppContext::create(resources()).take();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    document::Document* loaded = other->documents().project().documents().front().get();
    const auto nets = other->schematic().nets(*loaded);
    ASSERT_EQ(nets.size(), 4u);
    EXPECT_EQ(nets[1].name, "Выход");
    EXPECT_TRUE(other->schematic().check(*loaded).empty());
    const auto* r1 = loaded->objectsOf<schematic::Element>().front();
    EXPECT_EQ(r1->libraryVersion, context->schematic().library().version);  // документ помнит версию набора
    EXPECT_FALSE(r1->graphics.empty());
    fs::remove_all(path);
}

// Номера цепей (требование СХ-3): каждая цепь получает свой номер, цепи при этом не сливаются.
TEST_F(Scheme, NetNumbersAreUniqueAndDoNotMergeNets) {
    ASSERT_TRUE(s().addWire(*doc, {{60, 40}, {90, 40}}).ok());  // ещё одна цепь, пока ни к чему не присоединённая
    const auto before = s().nets(*doc);
    std::size_t wired = 0;
    for (const auto& net : before) wired += !net.wires.empty() && net.name.empty() ? 1 : 0;
    ASSERT_GT(wired, 1u);

    auto numbered = s().numberNets(*doc);
    ASSERT_TRUE(numbered.ok()) << numbered.error().message;
    EXPECT_EQ(static_cast<std::size_t>(numbered.value()), wired);
    const auto after = s().nets(*doc);
    ASSERT_EQ(after.size(), before.size());
    std::set<std::string> names;
    for (std::size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(pinsOf(after[i]), pinsOf(before[i]));
        if (after[i].wires.empty()) continue;
        EXPECT_FALSE(after[i].name.empty());
        EXPECT_TRUE(names.insert(after[i].name).second) << "номер повторился: " << after[i].name;
    }
    EXPECT_FALSE(has(s().check(*doc), Issue::Kind::LabelConflict));
    // Номер не прячет ошибку: свободный конец проводника по-прежнему считается висящим.
    EXPECT_TRUE(has(s().check(*doc), Issue::Kind::DanglingWire));

    // Повторная нумерация ничего не добавляет; отмена убирает все номера одним шагом.
    EXPECT_EQ(s().numberNets(*doc).value(), 0);
    ASSERT_TRUE(doc->undo());
    const auto restored = s().nets(*doc);
    ASSERT_EQ(restored.size(), before.size());
    for (std::size_t i = 0; i < restored.size(); ++i) EXPECT_EQ(restored[i].name, before[i].name);
}
