// Тесты системы стандартов (требования ОБ-7, СТ-1, СТ-3, СТ-4, СТ-5, СТ-6, ЧЕР-1).
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <numbers>
#include <fstream>

#include "AppContext.h"
#include "cosmocad/document/Transaction.h"
#include "cosmocad/graphics/Scene.h"

using namespace cosmocad;
using document::Transaction;

namespace {

std::filesystem::path resources() { return COSMOCAD_TEST_RESOURCES; }

std::unique_ptr<app::AppContext> makeContext() {
    auto context = app::AppContext::create(resources());
    EXPECT_TRUE(context.ok());
    return context.take();
}

std::vector<std::string> ruleIds(const std::vector<standards::Violation>& violations) {
    std::vector<std::string> ids;
    for (const auto& v : violations) ids.push_back(v.ruleId);
    std::sort(ids.begin(), ids.end());
    return ids;
}

int count(const std::vector<standards::Violation>& violations, const std::string& rule) {
    return static_cast<int>(std::count_if(violations.begin(), violations.end(),
                                          [&](const auto& v) { return v.ruleId == rule; }));
}

}  // namespace

TEST(Profile, EskdLoadsWithExpectedParameters) {
    auto context = makeContext();
    auto eskd = context->standards().profile("eskd");
    ASSERT_TRUE(eskd.ok()) << eskd.error().message;
    const auto& p = eskd.value();
    ASSERT_NE(p.findFormat("A4"), nullptr);
    EXPECT_DOUBLE_EQ(p.findFormat("A4")->width, 210.0);
    EXPECT_DOUBLE_EQ(p.findFormat("A4")->height, 297.0);
    EXPECT_DOUBLE_EQ(p.findFormat("A0")->height, 1189.0);
    EXPECT_DOUBLE_EQ(p.frame.left, 20.0);
    EXPECT_DOUBLE_EQ(p.frame.right, 5.0);
    EXPECT_DOUBLE_EQ(p.titleBlock.width, 185.0);
    EXPECT_DOUBLE_EQ(p.titleBlock.height, 55.0);
    ASSERT_NE(p.findLineType("dash_dot"), nullptr);
    EXPECT_EQ(p.findLineType("solid_main")->lineClass, standards::LineClass::Main);
    EXPECT_FALSE(p.rules.empty());
}

TEST(Profile, SheetGeometryFollowsFormatAndOrientation) {
    auto context = makeContext();
    auto eskd = context->standards().profile("eskd").take();
    drawing::Sheet sheet;
    sheet.format = "A3";
    sheet.landscape = true;
    auto g = standards::sheetGeometry(sheet, eskd);
    ASSERT_TRUE(g.has_value());
    EXPECT_DOUBLE_EQ(g->width, 420.0);
    EXPECT_DOUBLE_EQ(g->height, 297.0);
    EXPECT_DOUBLE_EQ(g->frame.min.x, 20.0);
    EXPECT_DOUBLE_EQ(g->frame.max.x, 415.0);
    EXPECT_DOUBLE_EQ(g->frame.max.y, 292.0);
    EXPECT_DOUBLE_EQ(g->titleBlock.min.x, 415.0 - 185.0);
    EXPECT_DOUBLE_EQ(g->titleBlock.max.y, 5.0 + 55.0);
    sheet.format = "B5";
    EXPECT_FALSE(standards::sheetGeometry(sheet, eskd).has_value());
}

// СТ-4: на правильно оформленном чертеже лишних замечаний нет.
TEST(Check, DemoDrawingHasNoViolations) {
    auto context = makeContext();
    auto demo = context->createDemoDrawing();
    ASSERT_TRUE(demo.ok()) << demo.error().message;
    auto violations = context->standards().check(*demo.value());
    ASSERT_TRUE(violations.ok());
    for (const auto& v : violations.value()) ADD_FAILURE() << v.ruleId << ": " << v.message;
}

// СТ-4: каждое намеренно внесённое нарушение найдено, и ничего сверх них.
TEST(Check, FindsExactlyTheInjectedViolations) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    auto sheetId = doc.objectsOf<drawing::Sheet>().front()->id();

    core::Uuid outside = context->drawing().addLine(doc, {100, 100}, {400, 100}).value();  // за рамкой А4
    core::Uuid badText = context->drawing().addText(doc, {60, 120}, "Текст", 4.0).value();  // 4 мм нет в ряду
    core::Uuid thick = context->drawing().addLine(doc, {60, 110}, {90, 110}).value();
    core::Uuid unknownType = context->drawing().addLine(doc, {60, 105}, {90, 105}).value();
    {
        Transaction tx(doc, "inject");
        ASSERT_TRUE(tx.modifyAs<drawing::LineEntity>(thick, [](auto& l) { l.lineWidth = 2.0; }).ok());
        ASSERT_TRUE(tx.modifyAs<drawing::LineEntity>(unknownType, [](auto& l) { l.lineTypeId = "zigzag"; }).ok());
        ASSERT_TRUE(tx.modifyAs<drawing::Sheet>(sheetId, [](auto& s) { s.titleBlock["developer"].clear(); }).ok());
        tx.commit();
    }

    auto violations = context->standards().check(doc).take();
    EXPECT_EQ(ruleIds(violations), (std::vector<std::string>{"ESKD-LINE-002", "ESKD-LINE-003", "ESKD-SHEET-003",
                                                              "ESKD-TEXT-001", "ESKD-TITLE-001"}));
    auto objectOf = [&](const std::string& rule) {
        for (const auto& v : violations)
            if (v.ruleId == rule) return v.objectId;
        return core::Uuid();
    };
    EXPECT_EQ(objectOf("ESKD-SHEET-003"), outside);
    EXPECT_EQ(objectOf("ESKD-TEXT-001"), badText);
    EXPECT_EQ(objectOf("ESKD-LINE-002"), thick);
    EXPECT_EQ(objectOf("ESKD-LINE-003"), unknownType);
    EXPECT_EQ(objectOf("ESKD-TITLE-001"), sheetId);
    for (const auto& v : violations) {
        EXPECT_FALSE(v.message.empty());
        EXPECT_FALSE(v.reference.empty()) << v.ruleId;  // у каждого замечания — ссылка на пункт стандарта
    }

    // Отмена правок убирает замечания: проверка зависит только от документа.
    while (doc.undo() && doc.size() > 0) {
        if (context->standards().check(doc).take().empty()) break;
    }
    EXPECT_TRUE(context->standards().check(doc).take().empty());
}

TEST(Check, MainLineWidthOutOfRangeAndUnknownFormat) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    auto sheetId = doc.objectsOf<drawing::Sheet>().front()->id();
    {
        Transaction tx(doc, "bad sheet");
        ASSERT_TRUE(tx.modifyAs<drawing::Sheet>(sheetId, [](auto& s) {
                           s.mainLineWidth = 0.3;
                           s.format = "B5";
                       }).ok());
        tx.commit();
    }
    auto violations = context->standards().check(doc).take();
    EXPECT_EQ(count(violations, "ESKD-LINE-001"), 1);
    EXPECT_EQ(count(violations, "ESKD-SHEET-002"), 1);
    for (const auto& v : violations) {
        if (v.ruleId == "ESKD-LINE-001") {
            EXPECT_EQ(v.severity, standards::Severity::Error);
        }
    }
}

TEST(Check, DocumentWithoutProfileIsAnErrorNotACrash) {
    auto context = makeContext();
    document::Document doc(document::DocumentKind::Drawing, "без профиля");
    auto result = context->standards().check(doc);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidState);

    doc.profileId = "no-such-profile";
    EXPECT_EQ(context->standards().check(doc).error().code, core::ErrorCode::NotFound);
}

// СТ-5: профиль организации наследует ЕСКД и меняет отдельные параметры без программирования.
TEST(Profile, CompanyProfileInheritsOverridesDisablesAndAppends) {
    auto context = makeContext();
    auto base = context->standards().profile("eskd").take();
    auto company = context->standards().profile("example-company");
    ASSERT_TRUE(company.ok()) << company.error().message;
    const auto& p = company.value();

    EXPECT_EQ(p.sheetFormats.size(), base.sheetFormats.size());  // унаследовано
    EXPECT_DOUBLE_EQ(p.titleBlock.width, 185.0);                 // унаследовано
    EXPECT_EQ(p.titleBlock.requiredFields.size(), 4u);           // переопределено
    auto has = [&](const std::string& id) {
        return std::any_of(p.rules.begin(), p.rules.end(), [&](const auto& r) { return r.id == id; });
    };
    EXPECT_FALSE(has("ESKD-GEOM-001"));   // отключено
    EXPECT_TRUE(has("COMPANY-LINE-001")); // добавлено
    EXPECT_TRUE(has("ESKD-LINE-001"));    // остальное на месте
    EXPECT_EQ(p.rules.size(), base.rules.size());  // минус одно, плюс одно

    // Смена профиля меняет результат проверки без пересборки программы (ОБ-7).
    document::Document& doc = *context->createDemoDrawing().value();
    {
        Transaction tx(doc, "s");
        auto sheetId = doc.objectsOf<drawing::Sheet>().front()->id();
        ASSERT_TRUE(tx.modifyAs<drawing::Sheet>(sheetId, [](auto& s) { s.mainLineWidth = 1.2; }).ok());
        tx.commit();
    }
    EXPECT_TRUE(context->standards().check(doc).take().empty());  // по ЕСКД 1,2 мм допустимо
    doc.profileId = "example-company";
    EXPECT_EQ(ruleIds(context->standards().check(doc).take()), (std::vector<std::string>{"COMPANY-LINE-001"}));
}

TEST(Profile, UserDirectoryOverridesShippedProfileAndBadProfilesAreReported) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("cosmocad-profiles-" + core::Uuid::generate().toString());
    fs::create_directories(dir / "mine");
    {
        std::ofstream out(dir / "mine" / "profile.json");
        out << R"({"id":"mine","extends":"eskd","version":"9","rulesAppend":[
                 {"id":"MINE-1","type":"no_such_check","severity":"error","message":"m"}]})";
    }
    auto context = app::AppContext::create(resources(), dir);
    ASSERT_TRUE(context.ok()) << context.error().message;
    auto mine = context.value()->standards().profile("mine");
    ASSERT_TRUE(mine.ok());
    EXPECT_EQ(mine.value().version, "9");

    // Опечатка в виде проверки не отключает правило молча, а даёт замечание.
    document::Document& doc = *context.value()->createDemoDrawing().value();
    doc.profileId = "mine";
    auto violations = context.value()->standards().check(doc).take();
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations[0].ruleId, "MINE-1");

    // Повреждённый профиль — ошибка загрузки, а не сбой.
    fs::create_directories(dir / "broken");
    {
        std::ofstream out(dir / "broken" / "profile.json");
        out << "{ это не json";
    }
    auto broken = app::AppContext::create(resources(), dir);
    ASSERT_FALSE(broken.ok());
    EXPECT_EQ(broken.error().code, core::ErrorCode::ParseError);
    fs::remove_all(dir);
}

TEST(Profile, CyclicInheritanceIsDetected) {
    standards::ProfileRegistry registry;
    ASSERT_TRUE(registry.loadJson({{"id", "a"}, {"extends", "b"}}).ok());
    ASSERT_TRUE(registry.loadJson({{"id", "b"}, {"extends", "a"}}).ok());
    auto resolved = registry.resolve("a");
    ASSERT_FALSE(resolved.ok());
    EXPECT_EQ(resolved.error().code, core::ErrorCode::InvalidState);
    EXPECT_FALSE(registry.loadJson(nlohmann::json::array()).ok());
}

TEST(LineStyle, ResolvesWidthFromSheetLayerAndOverride) {
    auto context = makeContext();
    auto eskd = context->standards().profile("eskd").take();
    document::Document& doc = *context->createDemoDrawing().value();  // s = 0,8 мм

    const drawing::Entity* contour = nullptr;
    const drawing::Entity* axis = nullptr;
    for (const auto* e : doc.objectsOf<drawing::LineEntity>()) {
        auto style = standards::resolveLineStyle(doc, *e, eskd);
        if (style.lineTypeId == "solid_main") contour = e;
        if (style.lineTypeId == "dash_dot") axis = e;
    }
    ASSERT_NE(contour, nullptr);
    ASSERT_NE(axis, nullptr);
    EXPECT_DOUBLE_EQ(standards::resolveLineStyle(doc, *contour, eskd).width, 0.8);
    auto axisStyle = standards::resolveLineStyle(doc, *axis, eskd);
    EXPECT_DOUBLE_EQ(axisStyle.width, 0.4);  // тонкая линия — s/2
    EXPECT_FALSE(axisStyle.pattern.empty());
}

// ЧЕР-1: основная надпись целиком описана в профиле и рисуется по этому описанию.
TEST(TitleBlock, Form1GridIsConsistentAndDrawnFromProfile) {
    auto context = makeContext();
    auto eskd = context->standards().profile("eskd").take();
    const auto& tb = eskd.titleBlock;
    ASSERT_FALSE(tb.lines.empty());
    ASSERT_FALSE(tb.labels.empty());
    ASSERT_FALSE(tb.fields.empty());

    // Все линии, подписи и поля лежат внутри габарита 185 × 55 мм.
    for (const auto& l : tb.lines) {
        for (double x : {l.x1, l.x2}) EXPECT_TRUE(x >= 0.0 && x <= tb.width);
        for (double y : {l.y1, l.y2}) EXPECT_TRUE(y >= 0.0 && y <= tb.height);
        EXPECT_TRUE(l.x1 == l.x2 || l.y1 == l.y2);  // только горизонтали и вертикали
    }
    for (const auto& label : tb.labels) EXPECT_TRUE(label.x < tb.width && label.y < tb.height);

    // Столбцы левой части: 7, 10, 23, 15, 10 мм — вертикали на 7, 17, 40, 55, 65.
    for (double x : {7.0, 17.0, 40.0, 55.0, 65.0})
        EXPECT_TRUE(std::any_of(tb.lines.begin(), tb.lines.end(), [x](const auto& l) { return l.x1 == x && l.x2 == x; }))
            << "нет вертикали x = " << x;
    // Правая часть: наименование 70 мм, справа 50 мм — вертикаль на 135.
    EXPECT_TRUE(std::any_of(tb.lines.begin(), tb.lines.end(), [](const auto& l) { return l.x1 == 135 && l.x2 == 135; }));

    // Обязательные поля существуют среди полей надписи.
    for (const auto& required : tb.requiredFields)
        EXPECT_TRUE(std::any_of(tb.fields.begin(), tb.fields.end(), [&](const auto& f) { return f.key == required; }));

    // Сцена содержит все линии сетки, подписи и заполненные поля.
    document::Document& doc = *context->createDemoDrawing().value();
    auto scene = graphics::buildScene(doc, &eskd);
    auto hasText = [&](const std::string& text) {
        return std::any_of(scene.items.begin(), scene.items.end(), [&](const auto& item) {
            return item.kind == graphics::SceneItem::Kind::Text && item.text == text;
        });
    };
    EXPECT_TRUE(hasText("Разраб."));
    EXPECT_TRUE(hasText("Масштаб"));
    EXPECT_TRUE(hasText("КСМ.001.001"));
    EXPECT_TRUE(hasText("1:1"));
    std::size_t frameLines = std::count_if(scene.items.begin(), scene.items.end(), [](const auto& item) {
        return item.kind == graphics::SceneItem::Kind::Line && item.layer == "FRAME";
    });
    EXPECT_EQ(frameLines, 4 /*рамка*/ + 4 /*контур надписи*/ + tb.lines.size());
}

// Профиль организации может заменить основную надпись целиком, не трогая программу.
TEST(TitleBlock, CustomProfileCanRedefineTitleBlock) {
    standards::ProfileRegistry registry;
    ASSERT_TRUE(registry.loadDirectory(resources() / "standards").ok());
    ASSERT_TRUE(registry.loadJson(nlohmann::json::parse(R"({
        "id": "tiny", "extends": "eskd",
        "titleBlock": { "width": 100, "height": 20, "requiredFields": ["title"],
                        "lines": [[50, 0, 50, 20]], "labels": [], 
                        "fields": [{ "key": "title", "title": "Название", "x": 2, "y": 8, "height": 5 }] }
    })")).ok());
    auto tiny = registry.resolve("tiny");
    ASSERT_TRUE(tiny.ok()) << tiny.error().message;
    EXPECT_DOUBLE_EQ(tiny.value().titleBlock.width, 100.0);
    EXPECT_EQ(tiny.value().titleBlock.lines.size(), 1u);
    EXPECT_TRUE(tiny.value().titleBlock.labels.empty());
    EXPECT_EQ(tiny.value().titleBlock.fields.size(), 1u);
    EXPECT_EQ(tiny.value().sheetFormats.size(), 5u);  // остальное унаследовано
}

// Правила штриховки (ГОСТ 2.306-68) и высота текста в размерах радиуса и угла.
TEST(Check, HatchRulesAndDimensionTextHeights) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    auto& service = context->drawing();
    auto rect = service.addRectangle(doc, {60, 100}, {100, 120}).value();

    auto good = service.addHatch(doc, rect, std::numbers::pi / 4, 3.0).value();
    auto mirrored = service.addHatch(doc, rect, 3 * std::numbers::pi / 4, 3.0).value();  // наклон влево — тоже 45°
    auto sixty = service.addHatch(doc, rect, std::numbers::pi / 3, 3.0).value();
    EXPECT_TRUE(context->standards().check(doc).take().empty());
    (void)good; (void)mirrored; (void)sixty;

    auto tooDense = service.addHatch(doc, rect, std::numbers::pi / 4, 0.5).value();
    auto badAngle = service.addHatch(doc, rect, 0.2, 3.0).value();
    auto radial = service.addRadialDimension(doc, {80, 110}, 5, 1.0, false).value();
    {
        Transaction tx(doc, "bad height");
        ASSERT_TRUE(tx.modifyAs<drawing::RadialDimension>(radial, [](auto& d) { d.textHeight = 4.2; }).ok());
        tx.commit();
    }
    auto violations = context->standards().check(doc).take();
    EXPECT_EQ(ruleIds(violations), (std::vector<std::string>{"ESKD-HATCH-001", "ESKD-HATCH-002", "ESKD-TEXT-001"}));
    for (const auto& v : violations) {
        core::Uuid expected = v.ruleId == "ESKD-HATCH-001" ? tooDense : v.ruleId == "ESKD-HATCH-002" ? badAngle : radial;
        EXPECT_EQ(v.objectId, expected) << v.ruleId;
    }
}
