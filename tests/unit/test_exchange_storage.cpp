// Тесты обмена и хранилища (требования 2D-9, ОБМ-2, ФМТ-1…ФМТ-4, ПЛ-10, КАЧ-5).
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "AppContext.h"
#include "cosmocad/document/Transaction.h"
#include "cosmocad/exchange/Exchange.h"
#include "cosmocad/graphics/Scene.h"

using namespace cosmocad;
namespace fs = std::filesystem;

namespace {

fs::path resources() { return COSMOCAD_TEST_RESOURCES; }

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("cosmocad-test-" + core::Uuid::generate().toString());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::unique_ptr<app::AppContext> makeContext() { return app::AppContext::create(resources()).take(); }

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeAll(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

fs::path firstDocumentFile(const fs::path& project) {
    for (const auto& entry : fs::directory_iterator(project / "documents")) return entry.path();
    return {};
}

}  // namespace

// ---------- DXF ----------

TEST(Dxf, RoundTripPreservesGeometryExactly) {
    document::Document doc(document::DocumentKind::Drawing, "dxf");
    drawing::DrawingService impl;
    drawing::IDrawingService& service = impl;
    auto layer = service.addLayer(doc, "AXES", "solid_main").value();
    service.addLine(doc, {0.1, 0.2}, {123.456789012345, -98.7654321}, layer).value();
    service.addCircle(doc, {10, 20}, 7.25).value();
    service.addArc(doc, {5, 5}, 3.0, 0.5, 2.5).value();
    service.addText(doc, {1, 2}, "Hello DXF", 3.5).value();

    std::stringstream stream;
    ASSERT_TRUE(exchange::writeDxf(graphics::buildScene(doc, nullptr), stream).ok());
    auto imported = exchange::readDxf(stream);
    ASSERT_TRUE(imported.ok()) << imported.error().message;
    EXPECT_EQ(imported.value().skippedEntities, 0u);

    const drawing::LineEntity* line = nullptr;
    const drawing::CircleEntity* circle = nullptr;
    const drawing::ArcEntity* arc = nullptr;
    const drawing::TextEntity* text = nullptr;
    const drawing::Layer* importedLayer = nullptr;
    for (const auto& object : imported.value().objects) {
        if (auto* p = dynamic_cast<const drawing::LineEntity*>(object.get())) line = p;
        if (auto* p = dynamic_cast<const drawing::CircleEntity*>(object.get())) circle = p;
        if (auto* p = dynamic_cast<const drawing::ArcEntity*>(object.get())) arc = p;
        if (auto* p = dynamic_cast<const drawing::TextEntity*>(object.get())) text = p;
        if (auto* p = dynamic_cast<const drawing::Layer*>(object.get())) importedLayer = p;
    }
    ASSERT_TRUE(line && circle && arc && text && importedLayer);
    EXPECT_NEAR(line->end.x, 123.456789012345, 1e-9);
    EXPECT_NEAR(line->end.y, -98.7654321, 1e-9);
    EXPECT_EQ(line->layerId, importedLayer->id());
    EXPECT_EQ(importedLayer->name, "AXES");
    EXPECT_NEAR(circle->radius, 7.25, 1e-12);
    EXPECT_NEAR(arc->startAngle, 0.5, 1e-9);
    EXPECT_NEAR(arc->endAngle, 2.5, 1e-9);
    EXPECT_EQ(text->text, "Hello DXF");
    EXPECT_NEAR(text->height, 3.5, 1e-12);
}

TEST(Dxf, UnsupportedEntitiesAreCountedNotLost) {
    std::istringstream in("0\nSECTION\n2\nENTITIES\n0\nSPLINE\n8\n0\n0\nLINE\n10\n0\n20\n0\n11\n1\n21\n1\n0\nENDSEC\n0\nEOF\n");
    auto imported = exchange::readDxf(in);
    ASSERT_TRUE(imported.ok());
    EXPECT_EQ(imported.value().skippedEntities, 1u);
    EXPECT_EQ(imported.value().objects.size(), 1u);
}

TEST(Dxf, MalformedInputGivesErrorsNotCrashes) {
    for (const char* text : {"", "garbage", "0\nSECTION\n2\nHEADER\n0\nENDSEC\n0\nEOF\n",
                             "0\nSECTION\n2\nENTITIES\n0\nLINE\n10\nabc\n20\n0\n11\n1\n21\n1\n0\nENDSEC\n",
                             "0\nSECTION\n2\nENTITIES\n0\nCIRCLE\n10\n0\n20\n0\n0\nENDSEC\n",
                             "0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n1e999\n20\n0\n11\n1\n21\n1\n0\nENDSEC\n",
                             "0\nSECTION\n2\nENTITIES\n5\n"}) {
        std::istringstream in(text);
        EXPECT_FALSE(exchange::readDxf(in).ok()) << "вход: " << text;
    }
}

// ОБМ-2: случайно искажённые файлы не приводят к сбою программы.
TEST(Dxf, RandomlyCorruptedFilesNeverCrash) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    auto profile = context->profileFor(doc);
    std::stringstream good;
    ASSERT_TRUE(exchange::writeDxf(graphics::buildScene(doc, &*profile), good).ok());
    const std::string original = good.str();

    std::mt19937 rng(12345);
    int parsed = 0, rejected = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string mutated = original;
        int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) {
            std::size_t position = rng() % mutated.size();
            switch (rng() % 3) {
                case 0: mutated[position] = static_cast<char>(rng() % 256); break;
                case 1: mutated.erase(position, 1 + rng() % 20); break;
                default: mutated.insert(position, std::string(1 + rng() % 5, static_cast<char>(rng() % 256)));
            }
            if (mutated.empty()) mutated = "0";
        }
        if (rng() % 10 == 0) mutated.resize(rng() % mutated.size());
        std::istringstream in(mutated);
        auto result = exchange::readDxf(in);  // достаточно того, что вызов вернулся
        result.ok() ? ++parsed : ++rejected;
    }
    EXPECT_EQ(parsed + rejected, 2000);
    EXPECT_GT(rejected, 0);
}

TEST(Svg, ContainsSheetAndEscapesText) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    context->drawing().addText(doc, {60, 130}, "a < b & \"c\"", 3.5).value();
    auto profile = context->profileFor(doc);
    std::stringstream out;
    ASSERT_TRUE(exchange::writeSvg(graphics::buildScene(doc, &*profile), out).ok());
    std::string svg = out.str();
    EXPECT_NE(svg.find("width=\"210mm\""), std::string::npos);  // масштаб 1:1, лист А4
    EXPECT_NE(svg.find("height=\"297mm\""), std::string::npos);
    EXPECT_NE(svg.find("<circle"), std::string::npos);
    EXPECT_NE(svg.find("stroke-dasharray"), std::string::npos);  // осевые линии штрихпунктирные
    EXPECT_NE(svg.find("a &lt; b &amp; &quot;c&quot;"), std::string::npos);
    EXPECT_EQ(svg.find("a < b"), std::string::npos);
}

// ---------- Хранилище ----------

TEST(Storage, SaveLoadRoundTripIsLossless) {
    TempDir tmp;
    auto context = makeContext();
    context->documents().newProject("Проект");
    document::Document& doc = *context->createDemoDrawing().value();
    const std::string before = doc.toJson().dump();
    const core::Uuid projectId = context->documents().project().id();
    fs::path path = tmp.path / "p.cosmocad";

    ASSERT_TRUE(context->documents().saveProject(path).ok());
    EXPECT_FALSE(doc.isModified());
    EXPECT_TRUE(fs::exists(path / "manifest.json"));

    auto other = makeContext();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    ASSERT_EQ(other->documents().project().documents().size(), 1u);
    EXPECT_EQ(other->documents().project().id(), projectId);
    EXPECT_EQ(other->documents().project().name, "Проект");
    EXPECT_EQ(other->documents().project().documents()[0]->toJson().dump(), before);
    // Профиль сохраняется вместе с документом (СТ-1).
    EXPECT_EQ(other->documents().project().documents()[0]->profileId, "eskd");
    EXPECT_TRUE(other->standards().check(*other->documents().project().documents()[0]).take().empty());
}

TEST(Storage, HundredDocumentsKeepOrder) {
    TempDir tmp;
    auto context = makeContext();
    std::vector<core::Uuid> ids;
    for (int i = 0; i < 100; ++i) {
        auto doc = context->documents().createDocument(document::DocumentKind::Drawing, "Д" + std::to_string(i));
        ASSERT_TRUE(doc.ok());
        context->drawing().addLine(*doc.value(), {0, 0}, {1.0 + i, 1}).value();
        ids.push_back(doc.value()->id());
    }
    fs::path path = tmp.path / "many.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto other = makeContext();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    const auto& documents = other->documents().project().documents();
    ASSERT_EQ(documents.size(), 100u);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(documents[i]->id(), ids[i]);
}

TEST(Storage, CorruptionIsDetectedAndNamed) {
    TempDir tmp;
    auto context = makeContext();
    context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());

    // Меняем одну цифру в документе: файл остаётся правильным JSON, но сумма не сходится.
    fs::path file = firstDocumentFile(path);
    std::string content = readAll(file);
    auto position = content.find("170.0");
    ASSERT_NE(position, std::string::npos);
    content[position] = '2';
    writeAll(file, content);
    auto result = makeContext()->documents().openProject(path);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, core::ErrorCode::CorruptedData);
    EXPECT_NE(result.error().message.find(file.filename().string()), std::string::npos);  // назван повреждённый файл
}

TEST(Storage, RandomByteDamageNeverCrashes) {
    TempDir tmp;
    auto context = makeContext();
    context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    fs::path documentFile = firstDocumentFile(path);
    const std::string manifest = readAll(path / "manifest.json");
    const std::string document = readAll(documentFile);

    std::mt19937 rng(777);
    int opened = 0;
    for (int i = 0; i < 300; ++i) {
        bool damageManifest = rng() % 2 == 0;
        std::string damaged = damageManifest ? manifest : document;
        for (int e = 0; e < 3; ++e) damaged[rng() % damaged.size()] = static_cast<char>(rng() % 256);
        if (rng() % 5 == 0) damaged.resize(rng() % damaged.size());
        writeAll(damageManifest ? path / "manifest.json" : documentFile, damaged);
        if (makeContext()->documents().openProject(path).ok()) ++opened;
        writeAll(path / "manifest.json", manifest);
        writeAll(documentFile, document);
    }
    // Повреждение документа обнаруживается всегда; опись не защищена суммой,
    // поэтому отдельные искажения её необязательных полей допустимы.
    EXPECT_LT(opened, 300);
    EXPECT_TRUE(makeContext()->documents().openProject(path).ok());  // исходный файл цел
}

TEST(Storage, NewerFormatVersionIsRefusedWithClearMessage) {
    TempDir tmp;
    auto context = makeContext();
    context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    auto manifest = nlohmann::json::parse(readAll(path / "manifest.json"));
    manifest["formatVersion"] = storage::kFormatVersion + 1;
    writeAll(path / "manifest.json", manifest.dump());
    auto result = makeContext()->documents().openProject(path);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, core::ErrorCode::UnsupportedVersion);
}

TEST(Storage, NotAProjectAndPathTraversalAreRejected) {
    TempDir tmp;
    EXPECT_EQ(makeContext()->documents().openProject(tmp.path / "missing.cosmocad").error().code,
              core::ErrorCode::NotFound);

    auto context = makeContext();
    context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    writeAll(tmp.path / "secret.json", "{}");
    auto manifest = nlohmann::json::parse(readAll(path / "manifest.json"));
    manifest["documents"][0]["file"] = "../secret.json";  // попытка выйти за каталог проекта
    writeAll(path / "manifest.json", manifest.dump());
    EXPECT_EQ(makeContext()->documents().openProject(path).error().code, core::ErrorCode::CorruptedData);

    manifest["format"] = "something-else";
    writeAll(path / "manifest.json", manifest.dump());
    EXPECT_EQ(makeContext()->documents().openProject(path).error().code, core::ErrorCode::CorruptedData);
}

// ФМТ-2: файл старой версии преобразуется цепочкой миграций.
TEST(Storage, MigrationChainUpgradesOldFiles) {
    TempDir tmp;
    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    fs::path path = tmp.path / "old.cosmocad";

    document::Project project("Старый");
    auto& doc = project.addDocument(std::make_unique<document::Document>(document::DocumentKind::Drawing, "d"));
    drawing::DrawingService impl;
    static_cast<drawing::IDrawingService&>(impl).addLine(doc, {0, 0}, {10, 0}).value();
    storage::ProjectStorage v1(factory, 1);
    ASSERT_TRUE(v1.save(project, path).ok());

    // «Версия 3» программы: в версии 2 документы получили приставку к имени, в версии 3 — отметку в описи.
    storage::ProjectStorage v3(factory, 3);
    EXPECT_EQ(v3.load(path).error().code, core::ErrorCode::UnsupportedVersion);  // миграции ещё не заданы
    int calls = 0;
    v3.registerMigration(1, [&](nlohmann::json&, std::map<std::string, nlohmann::json>& documents) {
        ++calls;
        for (auto& [file, json] : documents) json["name"] = "v2:" + json["name"].get<std::string>();
        return core::Result<void>();
    });
    v3.registerMigration(2, [&](nlohmann::json& manifest, std::map<std::string, nlohmann::json>&) {
        ++calls;
        manifest["projectName"] = manifest["projectName"].get<std::string>() + " (v3)";
        return core::Result<void>();
    });
    auto loaded = v3.load(path);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(loaded.value()->name, "Старый (v3)");
    EXPECT_EQ(loaded.value()->documents()[0]->name, "v2:d");
    EXPECT_EQ(loaded.value()->documents()[0]->size(), 1u);  // данные не потеряны
}

// ФМТ-4, ПЛ-10: прерванное сохранение не уничтожает предыдущую версию.
TEST(Storage, InterruptedSaveRecoversPreviousVersion) {
    TempDir tmp;
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    const std::string saved = doc.toJson().dump();

    // Имитация сбоя между двумя переименованиями: старая версия уже отодвинута,
    // новая ещё не на месте (остался недописанный временный каталог).
    fs::rename(path, fs::path(path.string() + ".bak-crash"));
    fs::create_directories(fs::path(path.string() + ".tmp-crash") / "documents");

    auto other = makeContext();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    EXPECT_EQ(other->documents().project().documents()[0]->toJson().dump(), saved);
}

TEST(Storage, SavingOverExistingProjectReplacesItAndLeavesNoTemporaries) {
    TempDir tmp;
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    context->drawing().addCircle(doc, {100, 100}, 5).value();
    ASSERT_TRUE(context->documents().saveProject(path).ok());

    int entries = 0;
    for (const auto& entry : fs::directory_iterator(tmp.path)) {
        ++entries;
        EXPECT_EQ(entry.path().filename(), "p.cosmocad");
    }
    EXPECT_EQ(entries, 1);
    auto other = makeContext();
    ASSERT_TRUE(other->documents().openProject(path).ok());
    EXPECT_EQ(other->documents().project().documents()[0]->toJson().dump(), doc.toJson().dump());
}

TEST(Storage, AutosaveIsDetectedAndClearedByNormalSave) {
    TempDir tmp;
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    fs::path path = tmp.path / "p.cosmocad";
    ASSERT_TRUE(context->documents().saveProject(path).ok());
    EXPECT_FALSE(storage::ProjectStorage::hasNewerAutosave(path));

    context->drawing().addCircle(doc, {100, 100}, 5).value();
    ASSERT_TRUE(context->storage().saveAutosave(context->documents().project(), path).ok());
    // Делаем автосохранение заведомо новее (время файлов на некоторых системах грубое).
    fs::last_write_time(storage::ProjectStorage::autosavePath(path) / "manifest.json",
                        fs::last_write_time(path / "manifest.json") + std::chrono::seconds(5));
    EXPECT_TRUE(storage::ProjectStorage::hasNewerAutosave(path));

    // Автосохранение содержит несохранённую работу.
    auto recovered = makeContext();
    ASSERT_TRUE(recovered->documents().openProject(storage::ProjectStorage::autosavePath(path)).ok());
    EXPECT_EQ(recovered->documents().project().documents()[0]->toJson().dump(), doc.toJson().dump());

    ASSERT_TRUE(context->documents().saveProject(path).ok());
    EXPECT_FALSE(storage::ProjectStorage::hasNewerAutosave(path));
}

TEST(Storage, Crc32MatchesKnownValue) {
    EXPECT_EQ(storage::crc32("123456789"), 0xCBF43926u);  // общеизвестное контрольное значение CRC-32
    EXPECT_EQ(storage::crc32(""), 0u);
}

// Защита от повторения найденных замедлений: раньше построение сцены, проверка оформления
// и отмена массовой вставки росли как квадрат числа объектов (минуты на 100 000 объектов).
TEST(Performance, LargeDrawingOperationsScaleLinearly) {
    auto context = makeContext();
    document::Document& doc = *context->createDemoDrawing().value();
    auto profile = context->profileFor(doc);
    const int count = 30000;
    {
        document::Transaction tx(doc, "bulk");
        for (int i = 0; i < count; ++i) {
            auto line = std::make_unique<drawing::LineEntity>();
            line->start = {30.0 + (i % 150), 70.0 + (i / 150)};
            line->end = {line->start.x + 0.5, line->start.y + 0.5};
            tx.add(std::move(line)).value();
        }
        tx.commit();
    }
    auto start = std::chrono::steady_clock::now();
    auto scene = graphics::buildScene(doc, &*profile);
    auto violations = context->standards().check(doc).take();
    ASSERT_TRUE(doc.undo());
    ASSERT_TRUE(doc.redo());
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_GT(scene.items.size(), static_cast<std::size_t>(count));
    EXPECT_TRUE(violations.empty());
    // Порог с большим запасом: на проверочной машине всё вместе занимает около 0,1 с.
    EXPECT_LT(seconds, 5.0);
}
