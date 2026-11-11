// Библиотека стандартных изделий: крепёж (требование БИБ-1).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>

#include "AppContext.h"

namespace fs = std::filesystem;
using namespace cosmocad;
using libraries::FastenerKind;

namespace {

// Свои материалы в тестах лежат во временной папке, а не в настоящей папке пользователя.
const fs::path kUserDirectory = [] {
    const fs::path directory = fs::temp_directory_path() / "cosmocad_test_user";
    fs::remove_all(directory);
    setenv("COSMOCAD_USER_DIR", directory.string().c_str(), 1);
    return directory;
}();

fs::path resources() { return fs::path(COSMOCAD_TEST_RESOURCES); }

struct Library : ::testing::Test {
    std::unique_ptr<app::AppContext> context = app::AppContext::create(resources()).take();
    const libraries::FastenerLibrary& library() { return context->fasteners(); }
    double volumeOf(document::Document& part) { return context->modeling().mass(part).value().geometry.volume; }
};

}  // namespace

TEST_F(Library, TablesAreLoadedWithSourceAndRanges) {
    EXPECT_FALSE(library().source().empty());
    EXPECT_EQ(library().bolts().size(), 15u);  // М6…М48
    EXPECT_EQ(library().nuts().size(), 15u);
    EXPECT_EQ(library().diameters(FastenerKind::Washer).size(), 17u);  // от 6 до 48, включая 33, 36 и 39
    // Болт М6 бывает длиной от 8 до 90 мм, М12 — от 14 мм.
    const auto m6 = library().boltLengths(6);
    EXPECT_DOUBLE_EQ(m6.front(), 8.0);
    EXPECT_DOUBLE_EQ(m6.back(), 90.0);
    EXPECT_DOUBLE_EQ(library().boltLengths(12).front(), 14.0);
    EXPECT_TRUE(library().boltLengths(7).empty());
}

TEST_F(Library, DesignationThreadLengthAndRefusals) {
    auto bolt = library().describe({FastenerKind::Bolt, 12, 60});
    ASSERT_TRUE(bolt.ok()) << bolt.error().message;
    EXPECT_EQ(bolt.value().designation, "Болт М12×60 ГОСТ 7798-70");
    EXPECT_DOUBLE_EQ(bolt.value().threadLength, 30.0);
    EXPECT_FALSE(bolt.value().notRecommended);
    // Короткий болт — резьба на всём стержне; длинный — длина резьбы больше.
    EXPECT_DOUBLE_EQ(library().describe({FastenerKind::Bolt, 12, 30}).value().threadLength, 30.0);
    EXPECT_DOUBLE_EQ(library().describe({FastenerKind::Bolt, 12, 32}).value().threadLength, 32.0);
    EXPECT_DOUBLE_EQ(library().describe({FastenerKind::Bolt, 12, 150}).value().threadLength, 36.0);
    // Диаметры и длины в скобках — «не рекомендуется».
    EXPECT_TRUE(library().describe({FastenerKind::Bolt, 14, 60}).value().notRecommended);
    EXPECT_TRUE(library().describe({FastenerKind::Bolt, 12, 85}).value().notRecommended);
    EXPECT_EQ(library().describe({FastenerKind::Nut, 12, 0}).value().designation, "Гайка М12 ГОСТ 5915-70");
    EXPECT_EQ(library().describe({FastenerKind::Washer, 12, 0}).value().designation, "Шайба 12 ГОСТ 11371-78");

    // Чего в стандарте нет — понятный отказ.
    EXPECT_FALSE(library().describe({FastenerKind::Bolt, 12, 61}).ok());
    EXPECT_FALSE(library().describe({FastenerKind::Bolt, 12, 10}).ok());   // короче наименьшей длины
    EXPECT_FALSE(library().describe({FastenerKind::Bolt, 6, 100}).ok());   // длиннее наибольшей
    EXPECT_FALSE(library().describe({FastenerKind::Bolt, 13, 60}).ok());
    EXPECT_TRUE(library().describe({FastenerKind::Washer, 36, 0}).ok());
    EXPECT_FALSE(library().describe({FastenerKind::Washer, 45, 0}).ok());
}

TEST_F(Library, PartsHaveStandardSizes) {
    // Болт М12×60: шестигранник под ключ 18, высота головки 7,5; стержень ⌀12 длиной 60.
    auto bolt = context->standardPart({FastenerKind::Bolt, 12, 60});
    ASSERT_TRUE(bolt.ok()) << bolt.error().message;
    const double hexagon = 18.0 * 18.0 * std::sqrt(3.0) / 2.0;
    EXPECT_NEAR(volumeOf(*bolt.value()), hexagon * 7.5 + std::numbers::pi * 36.0 * 60.0, 1e-3);
    auto box = context->kernel().bounds(context->modeling().body(*bolt.value()));
    ASSERT_TRUE(box.ok());
    EXPECT_NEAR(box.value().min.z, -7.5, 1e-6);  // опорная плоскость головки — z = 0
    EXPECT_NEAR(box.value().max.z, 60.0, 1e-6);
    EXPECT_NEAR(box.value().max.y - box.value().min.y, 18.0, 1e-6);  // размер под ключ
    EXPECT_EQ(bolt.value()->name, "Болт М12×60 ГОСТ 7798-70");

    // Гайка М12: под ключ 18, высота 10,8, отверстие ⌀12. Шайба 12: ⌀13 × ⌀24 × 2,5.
    auto nut = context->standardPart({FastenerKind::Nut, 12, 0});
    ASSERT_TRUE(nut.ok()) << nut.error().message;
    EXPECT_NEAR(volumeOf(*nut.value()), (hexagon - std::numbers::pi * 36.0) * 10.8, 1e-3);
    auto washer = context->standardPart({FastenerKind::Washer, 12, 0});
    ASSERT_TRUE(washer.ok()) << washer.error().message;
    EXPECT_NEAR(volumeOf(*washer.value()), std::numbers::pi * (144.0 - 42.25) * 2.5, 1e-3);

    // Повторный запрос того же изделия не создаёт второй документ.
    const std::size_t documents = context->documents().project().documents().size();
    auto again = context->standardPart({FastenerKind::Bolt, 12, 60});
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), bolt.value());
    EXPECT_EQ(context->documents().project().documents().size(), documents);
    EXPECT_FALSE(context->standardPart({FastenerKind::Bolt, 12, 61}).ok());
    EXPECT_EQ(context->documents().project().documents().size(), documents);
}

TEST_F(Library, FastenersGoIntoAssemblyAndBillOfMaterials) {
    auto demo = context->createDemoModel();
    ASSERT_TRUE(demo.ok());
    auto bolt = context->standardPart({FastenerKind::Bolt, 6, 25});
    auto washer = context->standardPart({FastenerKind::Washer, 6, 0});
    ASSERT_TRUE(bolt.ok());
    ASSERT_TRUE(washer.ok());
    auto& assembly = context->assembly();
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(assembly.addComponent(*demo.value().assembly, bolt.value()->id(), geometry::Placement::translation(100.0 + 20 * i, 0, 0)).ok());
        ASSERT_TRUE(assembly.addComponent(*demo.value().assembly, washer.value()->id(), geometry::Placement::translation(100.0 + 20 * i, 30, 0)).ok());
    }
    auto bom = assembly.billOfMaterials(*demo.value().assembly);
    ASSERT_TRUE(bom.ok());
    int bolts = 0, washers = 0;
    for (const auto& line : bom.value()) {
        if (line.designation == "Болт М6×25 ГОСТ 7798-70") bolts = line.quantity;
        if (line.designation == "Шайба 6 ГОСТ 11371-78") washers = line.quantity;
    }
    EXPECT_EQ(bolts, 4);
    EXPECT_EQ(washers, 4);

    // Проект с крепежом сохраняется и открывается; детали перестраиваются.
    const fs::path path = fs::temp_directory_path() / "cosmocad_fasteners.cosmocad";
    fs::remove_all(path);
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto other = app::AppContext::create(resources()).take();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    ASSERT_TRUE(other->rebuildAll().ok());
    bool found = false;
    for (const auto& document : other->documents().project().documents())
        if (document->name == "Болт М6×25 ГОСТ 7798-70") {
            found = true;
            EXPECT_GT(other->modeling().mass(*document).value().geometry.volume, 700.0);
        }
    EXPECT_TRUE(found);
    fs::remove_all(path);
}

TEST(LibraryFile, BadFileIsRejectedWithReason) {
    const fs::path path = fs::temp_directory_path() / "cosmocad_bad_fasteners.json";
    {
        std::ofstream out(path);
        out << R"({"version": "1", "bolts": {"standard": "X", "lengths": [10], "items": [{"d": 6, "pitch": 1, "S": 5, "k": 4, "e": 10,
                 "lengthMin": 8, "lengthMax": 90, "fullThreadUpTo": 20, "threadTo125": 18}]},
                 "nuts": {"standard": "Y", "items": []}, "washers": {"standard": "Z", "items": []}})";
    }
    auto bad = libraries::FastenerLibrary::load(path);  // размер под ключ меньше диаметра резьбы
    ASSERT_FALSE(bad.ok());
    EXPECT_NE(bad.error().message.find("М6"), std::string::npos);
    EXPECT_FALSE(libraries::FastenerLibrary::load(path.string() + ".нет").ok());
    fs::remove(path);
}

// Параметр детали → размер на чертеже (формула) → контур → эскиз детали → тело (требование ЭС-2).
TEST_F(Library, PartParameterDrivesDrawingConstraintAndBody) {
    using sketcher::ConstraintKind;
    using sketcher::PointRole;
    auto drawingDocument = context->documents().createDocument(document::DocumentKind::Drawing, "Контур");
    auto partDocument = context->documents().createDocument(document::DocumentKind::Part, "Планка");
    ASSERT_TRUE(drawingDocument.ok());
    ASSERT_TRUE(partDocument.ok());
    document::Document& sheet = *drawingDocument.value();
    document::Document& part = *partDocument.value();
    auto& draw = context->drawing();
    auto& sketch = context->sketcher();
    const std::vector<core::Uuid> s = {draw.addLine(sheet, {0, 0}, {58, 2}).value(), draw.addLine(sheet, {58, 2}, {61, 33}).value(),
                                       draw.addLine(sheet, {61, 33}, {1, 36}).value(), draw.addLine(sheet, {1, 36}, {0, 0}).value()};
    for (std::size_t i = 0; i < 4; ++i)
        ASSERT_TRUE(sketch.addConstraint(sheet, {ConstraintKind::Coincident, {{s[i], PointRole::End}, {s[(i + 1) % 4], PointRole::Start}}}).ok());
    for (const std::size_t i : {0u, 2u}) ASSERT_TRUE(sketch.addConstraint(sheet, {ConstraintKind::Horizontal, {{s[i]}}}).ok());
    for (const std::size_t i : {1u, 3u}) ASSERT_TRUE(sketch.addConstraint(sheet, {ConstraintKind::Vertical, {{s[i]}}}).ok());
    ASSERT_TRUE(sketch.addConstraint(sheet, {ConstraintKind::Length, {{s[1]}}, 35.0}).ok());

    // Длина контура — формула от параметра детали.
    ASSERT_TRUE(context->modeling().addParameter(part, "длина", "60").ok());
    auto value = context->evaluateWith(part, "длина + 0");
    ASSERT_TRUE(value.ok());
    auto driven = sketch.addConstraint(sheet, {ConstraintKind::Length, {{s[0]}}, value.value(), "длина + 0", part.id()});
    ASSERT_TRUE(driven.ok()) << driven.error().message;
    EXPECT_FALSE(context->evaluateWith(part, "нет_такого * 2").ok());
    EXPECT_FALSE(context->evaluateWith(sheet, "1").ok());  // параметры — только у детали

    // Эскиз детали снят с контура и помнит об этом.
    auto shapes = app::AppContext::contourFromDrawing(sheet, s);
    ASSERT_TRUE(shapes.ok()) << shapes.error().message;
    auto partSketch = context->modeling().addSketch(part, {"", "XY", "0", "", shapes.value(), sheet.id(), s});
    ASSERT_TRUE(partSketch.ok()) << partSketch.error().message;
    ASSERT_TRUE(context->modeling().addExtrude(part, {partSketch.value(), "10", false, false}).ok());
    ASSERT_TRUE(context->modeling().recompute(part).ok());
    EXPECT_NEAR(volumeOf(part), 60.0 * 35 * 10, 1e-3);
    const auto faces = context->kernel().faceNames(context->modeling().body(part));

    // Параметр изменился: размер на чертеже, контур и тело следуют за ним; имена граней прежние.
    ASSERT_TRUE(context->modeling().setParameter(part, "длина", "90").ok());
    auto synced = context->syncSketches(part);
    ASSERT_TRUE(synced.ok()) << synced.error().message;
    EXPECT_EQ(synced.value(), 1);
    EXPECT_NEAR(sheet.findAs<sketcher::Constraint>(driven.value())->value, 90.0, 1e-12);
    EXPECT_EQ(sheet.findAs<sketcher::Constraint>(driven.value())->expression, "длина + 0");
    const auto* bottom = sheet.findAs<drawing::LineEntity>(s[0]);
    EXPECT_NEAR(geometry::distance(bottom->start, bottom->end), 90.0, 1e-7);
    ASSERT_TRUE(context->modeling().recompute(part).ok());
    EXPECT_NEAR(volumeOf(part), 90.0 * 35 * 10, 1e-3);
    EXPECT_EQ(context->kernel().faceNames(context->modeling().body(part)), faces);
    EXPECT_EQ(context->syncSketches(part).value(), 0);  // повторно менять нечего
    // Размер изменён прямо на чертеже — эскиз детали следует за контуром.
    for (const auto* constraint : sketch.constraintsOf(sheet, s[1]))
        if (constraint->kind == ConstraintKind::Length) { ASSERT_TRUE(sketch.setValue(sheet, constraint->id(), 45.0).ok()); }
    EXPECT_EQ(context->syncSketches(part).value(), 1);
    ASSERT_TRUE(context->modeling().recompute(part).ok());
    EXPECT_NEAR(volumeOf(part), 90.0 * 45 * 10, 1e-3);

    // Связь переживает сохранение и открытие; изменение параметра после открытия работает так же.
    const fs::path path = fs::temp_directory_path() / "cosmocad_driven.cosmocad";
    fs::remove_all(path);
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto other = app::AppContext::create(resources()).take();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    document::Document* reopened = nullptr;
    for (const auto& document : other->documents().project().documents())
        if (document->name == "Планка") reopened = document.get();
    ASSERT_NE(reopened, nullptr);
    ASSERT_TRUE(other->modeling().setParameter(*reopened, "длина", "40").ok());
    ASSERT_TRUE(other->rebuildAll().ok());
    EXPECT_NEAR(other->modeling().mass(*reopened).value().geometry.volume, 40.0 * 45 * 10, 1e-3);
    fs::remove_all(path);

    // Значение, которое не уживается с другими ограничениями, — понятный отказ.
    ASSERT_TRUE(sketch.addConstraint(sheet, {ConstraintKind::Length, {{s[2]}}, 90.0}).ok());
    ASSERT_TRUE(context->modeling().setParameter(part, "длина", "120").ok());
    auto conflict = context->syncSketches(part);
    ASSERT_FALSE(conflict.ok());
    EXPECT_NE(conflict.error().message.find("Контур"), std::string::npos) << conflict.error().message;
}

// Библиотека материалов: встроенные с происхождением, свои, поиск, обмен (требования БИБ-2, БИБ-4).
TEST_F(Library, MaterialsBuiltInUserSearchAndExchange) {
    auto& materials = context->materials();
    EXPECT_FALSE(materials.source().empty());
    const auto* steel = materials.byTitle("Сталь 45 ГОСТ 1050-2013");
    ASSERT_NE(steel, nullptr);
    EXPECT_DOUBLE_EQ(steel->density, 7.85);
    EXPECT_FALSE(steel->note.empty());  // у каждой встроенной записи указано, откуда плотность
    for (const auto& material : materials.all()) EXPECT_FALSE(material.note.empty()) << material.name;
    EXPECT_NEAR(materials.byTitle("Д16")->density, 2.85 * 0.976, 1e-3);

    // Поиск по части названия, без учёта регистра и для кириллицы.
    const auto found = materials.find("сталь");
    EXPECT_GE(found.size(), 5u);
    for (const auto& material : found) EXPECT_NE(libraries::lowered(material.title()).find("сталь"), std::string::npos);
    EXPECT_EQ(materials.find("амг").size(), 4u);
    EXPECT_TRUE(materials.find("нет такого материала").empty());
    EXPECT_EQ(materials.find("").size(), materials.all().size());

    // Назначение материала детали: название и плотность попадают в свойства, масса пересчитывается.
    auto bolt = context->standardPart({FastenerKind::Bolt, 12, 60});
    ASSERT_TRUE(bolt.ok());
    const double steelMass = context->modeling().mass(*bolt.value()).value().massGrams;
    ASSERT_TRUE(context->setPartMaterial(*bolt.value(), "Д16").ok());
    EXPECT_NEAR(context->modeling().mass(*bolt.value()).value().massGrams, steelMass * (2.85 * 0.976) / 7.85, steelMass * 1e-3);
    EXPECT_EQ(bolt.value()->objectsOf<modeling::PartProperties>().front()->material, "Д16");
    EXPECT_EQ(bolt.value()->objectsOf<modeling::PartProperties>().front()->designation, "Болт М12×60 ГОСТ 7798-70");
    EXPECT_FALSE(context->setPartMaterial(*bolt.value(), "Неизвестный").ok());

    // Свой материал: сохраняется в папке пользователя и виден новому запуску программы.
    const std::size_t builtIn = materials.all().size();
    ASSERT_TRUE(materials.addUser({"Капролон", "ТУ 6-05-988-87", 1.15, "паспорт поставщика", true}).ok());
    EXPECT_FALSE(materials.addUser({"", "", 1.0, "", true}).ok());
    EXPECT_FALSE(materials.addUser({"Чудо-сплав", "", 0.0, "", true}).ok());
    EXPECT_EQ(materials.all().size(), builtIn + 1);
    EXPECT_TRUE(materials.byTitle("Капролон ТУ 6-05-988-87")->user);
    auto restarted = app::AppContext::create(resources()).take();
    ASSERT_NE(restarted->materials().byTitle("Капролон ТУ 6-05-988-87"), nullptr);
    EXPECT_DOUBLE_EQ(restarted->materials().byTitle("Капролон ТУ 6-05-988-87")->density, 1.15);

    // Обмен: свои материалы — в файл; из файла — к своим (в другой папке пользователя).
    const fs::path exchange = fs::temp_directory_path() / "cosmocad_materials_exchange.json";
    EXPECT_EQ(materials.exportUser(exchange).value(), 1);
    const fs::path otherUser = fs::temp_directory_path() / "cosmocad_other_user" / "materials.json";
    fs::remove_all(otherUser.parent_path());
    auto other = libraries::MaterialLibrary::load(resources() / "libraries" / "materials.json", otherUser);
    ASSERT_TRUE(other.ok());
    EXPECT_EQ(other.value().importUser(exchange).value(), 1);
    EXPECT_NE(other.value().byTitle("Капролон ТУ 6-05-988-87"), nullptr);
    EXPECT_TRUE(fs::exists(otherUser));
    EXPECT_FALSE(other.value().importUser(exchange.string() + ".нет").ok());

    // Удаляются только свои; встроенные — нет.
    ASSERT_TRUE(materials.removeUser("Капролон ТУ 6-05-988-87").ok());
    EXPECT_FALSE(materials.removeUser("Сталь 45 ГОСТ 1050-2013").ok());
    EXPECT_EQ(materials.all().size(), builtIn);
    fs::remove(exchange);
    fs::remove_all(otherUser.parent_path());
}

// Дополнения (требование ОБ-10): загрузка, команда, отказы, выключение.
TEST_F(Library, PluginsLoadRunAndFailSafely) {
    auto& plugins = context->plugins();
    EXPECT_TRUE(plugins.plugins().empty());
    // Рядом с настоящим дополнением — четыре испорченных: программа их отклоняет и работает дальше.
    const fs::path broken = fs::temp_directory_path() / "cosmocad_broken_plugins";
    fs::remove_all(broken);
    const auto manifest = [&](const std::string& folder, const std::string& text) {
        fs::create_directories(broken / folder);
        std::ofstream(broken / folder / "plugin.json") << text;
    };
    manifest("old-api", R"({"id": "old-api", "apiVersion": 999, "library": "x"})");
    manifest("garbage", "это не опись");
    manifest("no-library", R"({"id": "no-library", "apiVersion": 1, "library": "missing"})");
    manifest("bad.name", R"({"id": "bad.name", "apiVersion": 1, "library": "x"})");
    EXPECT_EQ(plugins.loadFrom(broken), 0);
    ASSERT_EQ(plugins.plugins().size(), 4u);
    const auto stateOf = [&](const std::string& id) {
        for (const auto& plugin : plugins.plugins())
            if (plugin.id == id) return plugin;
        return app::PluginInfo{};
    };
    EXPECT_EQ(stateOf("old-api").state, app::PluginState::Rejected);
    EXPECT_NE(stateOf("old-api").reason.find("999"), std::string::npos);
    EXPECT_EQ(stateOf("garbage").state, app::PluginState::Rejected);
    EXPECT_EQ(stateOf("no-library").state, app::PluginState::Failed);
    EXPECT_EQ(stateOf("bad.name").state, app::PluginState::Rejected);
    EXPECT_TRUE(plugins.commands().empty());
    EXPECT_EQ(plugins.loadFrom(broken / "нет такой папки"), 0);

    // Пример дополнения из поставки загружается и регистрирует команду.
    EXPECT_EQ(plugins.loadFrom(fs::path(COSMOCAD_TEST_PLUGINS)), 1);
    EXPECT_EQ(stateOf("mass-report").state, app::PluginState::Active);
    ASSERT_EQ(plugins.commands().size(), 1u);
    EXPECT_EQ(plugins.commands().front()->id, "mass-report.csv");
    // Повторная загрузка той же папки второго экземпляра не создаёт.
    EXPECT_EQ(plugins.loadFrom(fs::path(COSMOCAD_TEST_PLUGINS)), 0);
    EXPECT_EQ(plugins.commands().size(), 1u);

    // Команда работает с проектом: ведомость масс двух деталей примера.
    ASSERT_TRUE(context->createDemoModel().ok());
    auto table = plugins.run("mass-report.csv");
    ASSERT_TRUE(table.ok()) << table.error().message;
    EXPECT_NE(table.value().find("КСМ.003.001;Опора;"), std::string::npos) << table.value();
    EXPECT_NE(table.value().find("Всего деталей: 2"), std::string::npos) << table.value();
    const fs::path file = fs::temp_directory_path() / "cosmocad_mass_report.csv";
    auto saved = plugins.run("mass-report.csv", {{"path", file.string()}});
    ASSERT_TRUE(saved.ok());
    EXPECT_GT(fs::file_size(file), 100u);
    EXPECT_FALSE(plugins.run("mass-report.нет").ok());
    EXPECT_FALSE(plugins.run("mass-report.csv", {{"path", (file / "нельзя" / "x.csv").string()}}).ok());
    fs::remove(file);
    fs::remove_all(broken);

    // Выключенное пользователем дополнение не загружается.
    auto other = app::AppContext::create(resources()).take();
    EXPECT_EQ(other->plugins().loadFrom(fs::path(COSMOCAD_TEST_PLUGINS), {"mass-report"}), 0);
    ASSERT_EQ(other->plugins().plugins().size(), 1u);
    EXPECT_EQ(other->plugins().plugins().front().state, app::PluginState::Disabled);
    EXPECT_TRUE(other->plugins().commands().empty());
}

// Прокат: уголки равнополочные ГОСТ 8509-93.
TEST(Profiles, AnglesMatchStandardTableAndBuild) {
    auto context = app::AppContext::create(resources()).take();
    const auto& profiles = context->profiles();
    EXPECT_EQ(profiles.angles().size(), 89u);
    EXPECT_EQ(profiles.angleWidths().front(), 20.0);
    EXPECT_EQ(profiles.angleWidths().back(), 250.0);
    EXPECT_EQ(profiles.angleThicknesses(50), (std::vector<double>{3, 4, 5, 6}));
    // Каждая строка: площадь по размерам и масса 1 м при 7,85 г/см³ сходятся с таблицей стандарта.
    for (const auto& a : profiles.angles()) {
        const double area = (a.t * (2 * a.b - a.t) + (1 - std::numbers::pi / 4) * (a.R * a.R - 2 * a.r * a.r)) / 100.0;
        EXPECT_NEAR(area, a.area, 0.01 * a.area) << a.b << "x" << a.t;
        EXPECT_NEAR(a.area * 0.785, a.mass, 0.012 * a.mass) << a.b << "x" << a.t;
    }
    // Отрезок уголка: объём тела равен площади сечения из таблицы, умноженной на длину.
    auto part = context->anglePart(63, 5, 400);
    ASSERT_TRUE(part);
    EXPECT_EQ(part.value()->name, "Уголок 63×63×5 ГОСТ 8509-93 L=400");
    const double volume = context->kernel().massProperties(context->modeling().body(*part.value())).value().volume;
    EXPECT_NEAR(volume, profiles.angle(63, 5)->area * 100.0 * 400.0, 0.003 * volume);
    EXPECT_EQ(context->anglePart(63, 5, 400).value(), part.value());
    const std::size_t count = context->documents().project().documents().size();
    EXPECT_FALSE(context->anglePart(63, 7, 400));
    EXPECT_FALSE(context->anglePart(63, 5, 0));
    EXPECT_EQ(context->documents().project().documents().size(), count);
}

// Прокат: швеллеры ГОСТ 8240-97 и двутавры ГОСТ Р 57837-2017.
TEST(Profiles, ChannelsAndBeamsMatchStandardTablesAndBuild) {
    auto context = app::AppContext::create(resources()).take();
    const auto& profiles = context->profiles();
    const double k = 1 - std::numbers::pi / 4;
    EXPECT_EQ(profiles.channels().size(), 42u);
    for (const auto& c : profiles.channels()) {
        const double area = (c.h * c.s + 2 * (c.b - c.s) * c.t + 2 * k * (c.R * c.R - c.r * c.r)) / 100.0;
        EXPECT_NEAR(area, c.area, 0.01 * c.area) << c.name;
        EXPECT_NEAR(c.area * 0.785, c.mass, 0.012 * c.mass) << c.name;
    }
    EXPECT_EQ(profiles.beams().size(), 335u);
    EXPECT_EQ(profiles.beamTypes().size(), 6u);
    for (const auto& m : profiles.beams()) {
        const double area = (2 * m.b * m.t + (m.h - 2 * m.t) * m.s + 4 * k * m.r * m.r) / 100.0;
        EXPECT_NEAR(area, m.area, 0.006 * m.area) << m.name;
        if (m.mass > 0) {
            EXPECT_NEAR(m.area * 0.785, m.mass, 0.012 * m.mass) << m.name;
        }
    }
    EXPECT_EQ(profiles.beam("20Б0"), nullptr);  // исключён Изменением № 1
    ASSERT_NE(profiles.beam("80Ш1"), nullptr);  // добавлен Изменением № 1
    ASSERT_NE(profiles.channel("16аП"), nullptr);
    EXPECT_DOUBLE_EQ(profiles.channel("16аП")->b, 68.0);
    EXPECT_EQ(profiles.channel("14Л"), nullptr);  // размеры в стандарте не сходятся с площадью

    // Тело отрезка: объём равен площади сечения из таблицы, умноженной на длину.
    const auto volumeOf = [&](document::Document* part) {
        return context->kernel().massProperties(context->modeling().body(*part)).value().volume;
    };
    auto channel = context->profilePart("channel", "20П", 300);
    ASSERT_TRUE(channel);
    EXPECT_EQ(channel.value()->name, "Швеллер 20П ГОСТ 8240-97 L=300");
    EXPECT_NEAR(volumeOf(channel.value()), profiles.channel("20П")->area * 100 * 300, 0.006 * volumeOf(channel.value()));
    auto box = context->kernel().bounds(context->modeling().body(*channel.value()));
    EXPECT_NEAR(box.value().max.x - box.value().min.x, 76.0, 1e-6);
    EXPECT_NEAR(box.value().max.y - box.value().min.y, 200.0, 1e-6);
    auto beam = context->profilePart("beam", "30Ш1", 300);
    ASSERT_TRUE(beam);
    EXPECT_EQ(beam.value()->name, "Двутавр 30Ш1 ГОСТ Р 57837-2017 L=300");
    EXPECT_NEAR(volumeOf(beam.value()), profiles.beam("30Ш1")->area * 100 * 300, 0.006 * volumeOf(beam.value()));
    box = context->kernel().bounds(context->modeling().body(*beam.value()));
    EXPECT_NEAR(box.value().max.x - box.value().min.x, profiles.beam("30Ш1")->b, 1e-6);
    EXPECT_NEAR(box.value().max.y - box.value().min.y, profiles.beam("30Ш1")->h, 1e-6);
    const std::size_t count = context->documents().project().documents().size();
    EXPECT_FALSE(context->profilePart("beam", "20Б0", 300));
    EXPECT_FALSE(context->profilePart("channel", "20П", -1));
    EXPECT_FALSE(context->profilePart("rail", "20П", 300));
    EXPECT_EQ(context->documents().project().documents().size(), count);
}
