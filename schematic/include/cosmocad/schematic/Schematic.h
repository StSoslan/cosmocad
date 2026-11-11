// Электрические схемы: элементы с выводами, проводники, метки цепей,
// нумерация цепей, позиционные обозначения, перечень элементов, проверка схемы.
//
// Схема — это чертёж (лист, рамка, основная надпись), на котором лежат объекты схемы.
// Объекты схемы — наследники drawing::Entity, поэтому показ, выбор, печать и экспорт
// работают для них так же, как для линий чертежа.
//
// Правило соединения: проводники соединены, если конец одного лежит на другом или концы
// совпадают. Простое пересечение двух проводников соединением не считается.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cosmocad/document/Document.h"
#include "cosmocad/drawing/Entities.h"

namespace cosmocad::schematic {

using core::Result;
using core::Uuid;
using drawing::Primitive;
using geometry::BBox2;
using geometry::Point2;

struct Pin {
    std::string name;
    Point2 at;  // в координатах обозначения
};

// Условное графическое обозначение из набора профиля стандарта.
struct SymbolDef {
    std::string id, name, prefix, standard, source;
    std::vector<Pin> pins;
    std::vector<Primitive> graphics;  // отрезки, окружности, дуги в координатах обозначения
};

class SymbolLibrary {
public:
    static Result<SymbolLibrary> load(const std::filesystem::path& file);
    const SymbolDef* find(const std::string& id) const;
    std::string id, version;
    std::vector<SymbolDef> symbols;
};

// Элемент схемы. Хранит копию графики и выводов своего обозначения и версию набора,
// из которого оно взято: документ читается и без набора и не меняется при его обновлении.
class Element final : public document::ObjectT<Element, drawing::Entity> {
public:
    static constexpr std::string_view kType = "SchElement";
    std::string symbolId, symbolName, libraryVersion, prefix;
    std::string designator;  // позиционное обозначение: R1, C2
    std::string value;       // номинал или тип: «10 кОм»
    Point2 position;
    int rotation = 0;        // градусы: 0, 90, 180, 270
    std::vector<Pin> pins;
    std::vector<Primitive> graphics;
    double textHeight = 3.5;

    Point2 toSheet(Point2 local) const;
    Point2 pinPosition(std::size_t index) const { return toSheet(pins[index].at); }
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }
    std::vector<Primitive> primitives() const override;
    std::vector<Point2> snapPoints() const override;

    BBox2 bodyBounds() const;  // габарит без надписей

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Проводник — ломаная линия электрической связи.
class Wire final : public document::ObjectT<Wire, drawing::Entity> {
public:
    static constexpr std::string_view kType = "SchWire";
    std::vector<Point2> points;
    std::vector<Point2> junctions;  // где рисовать точки соединения (ведёт сервис схемы)

    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::vector<Primitive> primitives() const override;
    std::vector<Point2> snapPoints() const override { return points; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Метка цепи: имя цепи и знак того, что конец проводника уходит за пределы схемы.
class Label final : public document::ObjectT<Label, drawing::Entity> {
public:
    static constexpr std::string_view kType = "SchLabel";
    Point2 position;
    std::string text;
    double textHeight = 3.5;

    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }
    std::vector<Primitive> primitives() const override;
    std::vector<Point2> snapPoints() const override { return {position}; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

void registerTypes(document::ObjectFactory& factory);

struct NetPin {
    Uuid elementId;
    std::string designator, pin;
};

struct Net {
    int number = 0;          // номера идут подряд с 1
    std::string name;        // имя из метки цепи; пусто, если метки нет
    std::vector<NetPin> pins;
    std::vector<Uuid> wires;
};

struct ListRow {
    std::string designators;  // «R1», «R1, R2», «R1 ... R3» (ГОСТ 2.701-2008, п. 5.7)
    std::string name;         // «Резистор 10 кОм»
    int count = 0;
};

struct Issue {
    enum class Kind { UnconnectedPin, DanglingWire, DuplicateDesignator, LabelConflict, EmptyDesignator } kind;
    std::string message;
    Uuid objectId;
};

class SchematicService {
public:
    explicit SchematicService(SymbolLibrary library) : library_(std::move(library)) {}
    const SymbolLibrary& library() const { return library_; }

    // Позиционное обозначение выдаётся само: следующий свободный номер для буквы элемента.
    Result<Uuid> addElement(document::Document& document, const std::string& symbolId, Point2 position,
                            int rotationDegrees = 0, const std::string& value = {});
    Result<Uuid> addWire(document::Document& document, const std::vector<Point2>& points);
    Result<Uuid> addLabel(document::Document& document, Point2 position, const std::string& text);
    Result<void> setValue(document::Document& document, const Uuid& elementId, const std::string& value);
    // Повтор обозначения отклоняется.
    Result<void> setDesignator(document::Document& document, const Uuid& elementId, const std::string& designator);
    // Перенос элементов: концы проводников, присоединённые к их выводам, едут следом.
    Result<void> moveElements(document::Document& document, const std::vector<Uuid>& elementIds, Point2 delta);
    // Перенумерация по расположению на листе: слева направо, в столбце сверху вниз.
    // Возвращает число изменённых обозначений.
    Result<int> renumber(document::Document& document);
    // Нумерация цепей: каждая цепь с проводником и без имени получает метку с номером —
    // наименьшим свободным числом. Цепи с именем не трогаются. Возвращает число новых меток.
    Result<int> numberNets(document::Document& document);
    // Обновляет точки соединения на проводниках (после правки схемы обычными инструментами чертежа).
    Result<void> refreshJunctions(document::Document& document);

    std::vector<Net> nets(const document::Document& document) const;
    std::vector<ListRow> elementList(const document::Document& document) const;
    std::vector<Issue> check(const document::Document& document) const;
    // Перечень элементов в таблицу CSV (разделитель «;», UTF-8 с меткой).
    Result<void> exportElementListCsv(const document::Document& document, const std::filesystem::path& path) const;

private:
    SymbolLibrary library_;
};

}  // namespace cosmocad::schematic
