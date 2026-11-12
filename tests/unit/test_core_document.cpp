// Тесты базовых типов и модели документа (требования ОБ-8, ПЛ-1, ПЛ-2, ПЛ-5, ПЛ-6, ПЛ-7).
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <random>
#include <set>

#include "cosmocad/core/Units.h"
#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Transaction.h"
#include "cosmocad/drawing/Entities.h"

using namespace cosmocad;
using document::Document;
using document::DocumentKind;
using document::Transaction;
using drawing::CircleEntity;
using drawing::LineEntity;

TEST(Uuid, GenerateIsUniqueAndRoundTrips) {
    std::set<core::Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        core::Uuid id = core::Uuid::generate();
        EXPECT_FALSE(id.isNil());
        EXPECT_TRUE(seen.insert(id).second);
        auto parsed = core::Uuid::parse(id.toString());
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value(), id);
    }
}

TEST(Uuid, RejectsMalformedText) {
    EXPECT_FALSE(core::Uuid::parse("").ok());
    EXPECT_FALSE(core::Uuid::parse("not-a-uuid").ok());
    EXPECT_FALSE(core::Uuid::parse("00000000-0000-0000-0000-00000000000g").ok());
    EXPECT_FALSE(core::Uuid::parse("00000000x0000-0000-0000-000000000000").ok());
    EXPECT_TRUE(core::Uuid::parse("00000000-0000-0000-0000-000000000000").value().isNil());
}

TEST(Units, MillimetersAndInchesRoundTripWithoutDrift) {
    double value = 123.456;
    for (int i = 0; i < 1000; ++i) {
        value = core::toMillimeters(core::fromMillimeters(value, core::LengthUnit::Inch), core::LengthUnit::Inch);
    }
    EXPECT_NEAR(value, 123.456, 1e-9);
    EXPECT_DOUBLE_EQ(core::toMillimeters(1.0, core::LengthUnit::Inch), 25.4);
    EXPECT_NEAR(core::radiansToDegrees(core::degreesToRadians(37.5)), 37.5, 1e-12);
}

namespace {

std::unique_ptr<LineEntity> makeLine(double x1, double y1, double x2, double y2) {
    auto line = std::make_unique<LineEntity>();
    line->start = {x1, y1};
    line->end = {x2, y2};
    return line;
}

}  // namespace

TEST(Transaction, CommitAddsObjectsAndUndoRemovesThem) {
    Document doc(DocumentKind::Drawing, "t");
    core::Uuid id;
    {
        Transaction tx(doc, "add");
        auto added = tx.add(makeLine(0, 0, 10, 0));
        ASSERT_TRUE(added.ok());
        id = added.value();
        tx.commit();
    }
    EXPECT_EQ(doc.size(), 1u);
    EXPECT_TRUE(doc.isModified());
    ASSERT_TRUE(doc.undo());
    EXPECT_EQ(doc.size(), 0u);
    ASSERT_TRUE(doc.redo());
    ASSERT_NE(doc.findAs<LineEntity>(id), nullptr);
    EXPECT_DOUBLE_EQ(doc.findAs<LineEntity>(id)->end.x, 10.0);
}

TEST(Transaction, UncommittedChangesRollBackCompletely) {
    Document doc(DocumentKind::Drawing, "t");
    core::Uuid kept;
    {
        Transaction tx(doc, "seed");
        kept = tx.add(makeLine(0, 0, 1, 1)).value();
        tx.commit();
    }
    std::string before = doc.toJson().dump();
    {
        Transaction tx(doc, "abandoned");
        ASSERT_TRUE(tx.add(makeLine(5, 5, 6, 6)).ok());
        ASSERT_TRUE(tx.modifyAs<LineEntity>(kept, [](LineEntity& l) { l.end = {99, 99}; }).ok());
        ASSERT_TRUE(tx.remove(kept).ok());
        // нет commit — выход из области откатывает всё
    }
    EXPECT_EQ(doc.toJson().dump(), before);
    EXPECT_FALSE(doc.canRedo());
}

TEST(Transaction, SecondTransactionOnSameDocumentIsRejected) {
    Document doc(DocumentKind::Drawing, "t");
    Transaction first(doc, "first");
    Transaction second(doc, "second");
    EXPECT_TRUE(first.isActive());
    EXPECT_FALSE(second.isActive());
    auto result = second.add(makeLine(0, 0, 1, 1));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidState);
    EXPECT_FALSE(doc.undo());  // во время транзакции отмена недоступна
}

TEST(Transaction, ErrorsLeaveDocumentUntouched) {
    Document doc(DocumentKind::Drawing, "t");
    Transaction tx(doc, "errors");
    EXPECT_EQ(tx.remove(core::Uuid::generate()).error().code, core::ErrorCode::NotFound);
    EXPECT_EQ(tx.modify(core::Uuid::generate(), [](document::Object&) {}).error().code, core::ErrorCode::NotFound);
    EXPECT_EQ(tx.add(nullptr).error().code, core::ErrorCode::InvalidArgument);
    tx.commit();
    EXPECT_EQ(doc.size(), 0u);
    EXPECT_FALSE(doc.canUndo());  // пустая транзакция не попадает в историю
}

// ПЛ-6: 1000 случайных команд, затем отмена всех — документ совпадает с исходным.
TEST(UndoRedo, ThousandRandomCommandsUndoToOriginalAndRedoToFinal) {
    Document doc(DocumentKind::Drawing, "random");
    {
        Transaction tx(doc, "seed");
        for (int i = 0; i < 5; ++i) tx.add(makeLine(i, 0, i, 10)).value();
        tx.commit();
    }
    const std::string original = doc.toJson().dump();
    const std::size_t baseUndo = doc.journal().size();

    std::mt19937 rng(20261110);  // фиксированное зерно — тест воспроизводим
    std::uniform_real_distribution<double> coordinate(-500.0, 500.0);
    int commands = 0;
    while (commands < 1000) {
        std::vector<core::Uuid> ids;
        for (const auto& object : doc.objects()) ids.push_back(object->id());
        int action = static_cast<int>(rng() % 4);
        Transaction tx(doc, "cmd");
        if (action == 0 || ids.empty()) {
            tx.add(makeLine(coordinate(rng), coordinate(rng), coordinate(rng), coordinate(rng))).value();
        } else if (action == 1) {
            auto circle = std::make_unique<CircleEntity>();
            circle->center = {coordinate(rng), coordinate(rng)};
            circle->radius = 1.0 + std::abs(coordinate(rng));
            tx.add(std::move(circle)).value();
        } else if (action == 2) {
            ASSERT_TRUE(tx.remove(ids[rng() % ids.size()]).ok());
        } else {
            double dx = coordinate(rng);
            ASSERT_TRUE(tx.modify(ids[rng() % ids.size()], [dx](document::Object& object) {
                              object.name = "moved";
                              if (auto* line = dynamic_cast<LineEntity*>(&object)) line->start.x += dx;
                              if (auto* circle = dynamic_cast<CircleEntity*>(&object)) circle->center.x += dx;
                          }).ok());
        }
        tx.commit();
        ++commands;
    }
    const std::string final = doc.toJson().dump();
    EXPECT_NE(final, original);
    EXPECT_EQ(doc.journal().size(), baseUndo + 1000);  // ПЛ-7: каждая команда в журнале

    int undone = 0;
    while (undone < 1000 && doc.undo()) ++undone;
    EXPECT_EQ(undone, 1000);
    EXPECT_EQ(doc.toJson().dump(), original);

    while (doc.redo()) {
    }
    EXPECT_EQ(doc.toJson().dump(), final);
}

TEST(UndoRedo, NewCommandClearsRedoHistory) {
    Document doc(DocumentKind::Drawing, "t");
    for (int i = 0; i < 2; ++i) {
        Transaction tx(doc, "add");
        tx.add(makeLine(0, 0, 1, i + 1)).value();
        tx.commit();
    }
    ASSERT_TRUE(doc.undo());
    EXPECT_TRUE(doc.canRedo());
    Transaction tx(doc, "new");
    tx.add(makeLine(0, 0, 5, 5)).value();
    tx.commit();
    EXPECT_FALSE(doc.canRedo());
}

TEST(Document, JsonRoundTripPreservesIdsOrderAndAttributes) {
    document::ObjectFactory factory;
    drawing::registerTypes(factory);

    Document doc(DocumentKind::Drawing, "Чертёж");
    doc.profileId = "eskd";
    doc.profileVersion = "0.1.0";
    core::Uuid lineId;
    {
        Transaction tx(doc, "fill");
        auto layer = std::make_unique<drawing::Layer>();
        layer->name = "Оси";
        layer->lineTypeId = "dash_dot";
        core::Uuid layerId = tx.add(std::move(layer)).value();
        auto line = makeLine(1.5, 2.5, 3.5, 4.5);
        line->layerId = layerId;
        line->lineWidth = 0.35;
        line->attributes["material"] = std::string("Сталь 45");
        line->attributes["count"] = std::int64_t{3};
        line->attributes["mass"] = 1.25;
        line->attributes["checked"] = true;
        line->attributes["layerRef"] = layerId;
        lineId = tx.add(std::move(line)).value();
        tx.commit();
    }
    auto restored = Document::fromJson(doc.toJson(), factory);
    ASSERT_TRUE(restored.ok()) << restored.error().message;
    EXPECT_EQ(restored.value()->id(), doc.id());
    EXPECT_EQ(restored.value()->toJson().dump(), doc.toJson().dump());
    const auto* line = restored.value()->findAs<LineEntity>(lineId);
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(std::get<std::string>(line->attributes.at("material")), "Сталь 45");
    EXPECT_EQ(std::get<std::int64_t>(line->attributes.at("count")), 3);
    EXPECT_TRUE(std::holds_alternative<core::Uuid>(line->attributes.at("layerRef")));
    EXPECT_TRUE(restored.value()->findDanglingReferences().empty());
}

TEST(Document, KeepsUnknownTypesAndRejectsDuplicateIds) {
    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    Document doc(DocumentKind::Drawing, "t");
    {
        Transaction tx(doc, "add");
        tx.add(makeLine(0, 0, 1, 1)).value();
        tx.commit();
    }
    auto json = doc.toJson();
    auto duplicated = json;
    duplicated["objects"].push_back(duplicated["objects"][0]);
    EXPECT_EQ(Document::fromJson(duplicated, factory).error().code, core::ErrorCode::CorruptedData);

    // Объект неизвестного типа (например, из дополнения, которого сейчас нет) не теряется:
    // документ открывается, а при записи объект возвращается в файл слово в слово.
    auto unknown = json;
    unknown["objects"][0]["type"] = "Teapot";
    unknown["objects"][0]["spout"] = {{"length", 12.5}, {"curved", true}};
    auto kept = Document::fromJson(unknown, factory);
    ASSERT_TRUE(kept.ok()) << kept.error().message;
    ASSERT_EQ(kept.value()->objects().size(), 1u);
    EXPECT_EQ(kept.value()->objects().front()->typeName(), "Teapot");
    EXPECT_NE(dynamic_cast<const document::UnknownObject*>(kept.value()->objects().front().get()), nullptr);
    EXPECT_EQ(kept.value()->toJson()["objects"], unknown["objects"]);
    // Без номера объекта и такой объект — ошибка.
    auto broken = unknown;
    broken["objects"][0].erase("id");
    EXPECT_FALSE(Document::fromJson(broken, factory).ok());

    auto wrongType = json;
    wrongType["objects"][0]["start"] = "не точка";  // ПЛ-2: значение чужого типа — ошибка, а не порча данных
    EXPECT_FALSE(Document::fromJson(wrongType, factory).ok());
}

TEST(Document, DetectsDanglingReferences) {
    Document doc(DocumentKind::Drawing, "t");
    Transaction tx(doc, "add");
    auto line = makeLine(0, 0, 1, 1);
    line->attributes["ref"] = core::Uuid::generate();
    core::Uuid id = tx.add(std::move(line)).value();
    tx.commit();
    auto dangling = doc.findDanglingReferences();
    ASSERT_EQ(dangling.size(), 1u);
    EXPECT_EQ(dangling[0], id);
}
