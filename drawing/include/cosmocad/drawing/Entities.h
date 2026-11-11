// Объекты 2D-чертежа: слой, лист и геометрические элементы.
// Координаты — в миллиметрах, начало в левом нижнем углу листа, ось Y вверх.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cosmocad/document/Object.h"
#include "cosmocad/geometry/Geometry2D.h"

namespace cosmocad::drawing {

using core::Result;
using core::Uuid;
using geometry::BBox2;
using geometry::Point2;

// Слой: общий вид для группы элементов.
class Layer : public document::ObjectT<Layer> {
public:
    static constexpr std::string_view kType = "Layer";

    bool visible = true;
    bool locked = false;
    std::string color = "#000000";
    std::string lineTypeId = "solid_main";  // тип линии из профиля стандарта
    double lineWidth = 0.0;                 // мм; 0 — взять по типу линии

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Лист: формат, ориентация и поля основной надписи.
class Sheet : public document::ObjectT<Sheet> {
public:
    static constexpr std::string_view kType = "Sheet";

    std::string format = "A4";
    bool landscape = false;
    double mainLineWidth = 0.8;                      // толщина основной линии s, мм
    std::map<std::string, std::string> titleBlock;   // поля основной надписи

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Общая часть всех геометрических элементов.
// Простой графический элемент, из которых состоит составной объект (например, элемент схемы).
// Составной объект отдаёт такой список, и программа умеет его показать, выбрать и экспортировать,
// ничего не зная о самом объекте.
struct Primitive {
    enum class Kind { Line, Circle, Arc, Text, Dot } kind = Kind::Line;
    Point2 a, b;              // Line: концы; Circle, Arc, Dot: a — центр; Text: a — середина нижнего края
    double radius = 0.0;
    double startAngle = 0.0, endAngle = 0.0;
    std::string text;
    double textHeight = 3.5;
};

class Entity : public document::Object {
public:
    Uuid layerId;                             // пустой номер — слой по умолчанию
    std::optional<std::string> lineTypeId;    // переопределение типа линии слоя
    std::optional<double> lineWidth;          // переопределение толщины, мм

    virtual BBox2 bounds() const = 0;
    // Перенос, поворот, масштаб или отражение элемента.
    virtual void transform(const geometry::Transform2& t) = 0;
    // Высота текста элемента, если в нём есть текст (для проверки размера шрифта).
    virtual std::optional<double> textHeightValue() const { return std::nullopt; }
    // Графика составного объекта. Пусто — объект рисуется своим собственным способом.
    virtual std::vector<Primitive> primitives() const { return {}; }
    // Точки, к которым привязывается курсор (выводы элемента, концы проводника).
    virtual std::vector<Point2> snapPoints() const { return {}; }

protected:
    void writeEntityJson(nlohmann::json& json) const;
    Result<void> readEntityJson(const nlohmann::json& json);
};

class LineEntity : public document::ObjectT<LineEntity, Entity> {
public:
    static constexpr std::string_view kType = "Line";
    Point2 start, end;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

class CircleEntity : public document::ObjectT<CircleEntity, Entity> {
public:
    static constexpr std::string_view kType = "Circle";
    Point2 center;
    double radius = 1.0;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Дуга идёт против часовой стрелки от startAngle до endAngle (радианы).
class ArcEntity : public document::ObjectT<ArcEntity, Entity> {
public:
    static constexpr std::string_view kType = "Arc";
    Point2 center;
    double radius = 1.0;
    double startAngle = 0.0;
    double endAngle = 1.0;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

class TextEntity : public document::ObjectT<TextEntity, Entity> {
public:
    static constexpr std::string_view kType = "Text";
    Point2 position;       // левый нижний угол строки
    std::string text;
    double height = 3.5;   // высота шрифта, мм
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return height; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Дополнение к размерному числу: обозначение допуска или посадки и предельные отклонения.
// Примеры: suffix = "H7"; suffix = "±0,1"; upper = "+0,2", lower = "−0,1".
struct DimensionNote {
    std::string suffix;  // пишется сразу после числа тем же шрифтом
    std::string upper;   // верхнее отклонение — мелким шрифтом справа вверху
    std::string lower;   // нижнее отклонение — мелким шрифтом справа внизу

    bool empty() const { return suffix.empty() && upper.empty() && lower.empty(); }
    void write(nlohmann::json& json) const;
    void read(const nlohmann::json& json);
};

// Линейный размер между двумя точками. offset — отступ размерной линии
// от измеряемого отрезка (со знаком, по нормали слева от направления p1→p2).
class LinearDimension : public document::ObjectT<LinearDimension, Entity> {
public:
    static constexpr std::string_view kType = "LinearDimension";
    Point2 p1, p2;
    double offset = 10.0;
    double textHeight = 3.5;
    DimensionNote note;

    double value() const { return geometry::distance(p1, p2); }
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Размер радиуса или диаметра окружности либо дуги.
// angle — направление от центра к точке окружности, куда приходит стрелка.
class RadialDimension : public document::ObjectT<RadialDimension, Entity> {
public:
    static constexpr std::string_view kType = "RadialDimension";
    Point2 center;
    double radius = 1.0;
    double angle = 0.7853981633974483;  // 45°
    bool diameter = false;              // показывать диаметр, а не радиус
    double textHeight = 3.5;
    DimensionNote note;

    double value() const { return diameter ? 2.0 * radius : radius; }
    // У малого радиуса размерное число не помещается внутри: линия продолжается наружу,
    // и число пишется над продолжением. Возвращает длину продолжения (0 — число внутри).
    double outsideExtension() const { return !diameter && radius < 15.0 ? 12.0 : 0.0; }
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Угловой размер: угол с вершиной vertex от луча на p1 до луча на p2 против часовой стрелки.
class AngularDimension : public document::ObjectT<AngularDimension, Entity> {
public:
    static constexpr std::string_view kType = "AngularDimension";
    Point2 vertex, p1, p2;
    double arcRadius = 20.0;  // радиус размерной дуги
    double textHeight = 3.5;

    double valueDegrees() const;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Линия-выноска: стрелка в точке tip, излом в точке knee, от излома — горизонтальная полка с текстом.
class Leader : public document::ObjectT<Leader, Entity> {
public:
    static constexpr std::string_view kType = "Leader";
    Point2 tip, knee;
    std::string text;
    double textHeight = 3.5;

    // Конец полки: полка идёт от излома в сторону, противоположную стрелке.
    Point2 shelfEnd() const;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Знак шероховатости поверхности (ГОСТ 2.309-73).
//   Any       — способ обработки конструктором не установлен (черт. 2а);
//   Removal   — поверхность образуется удалением слоя материала (черт. 2б);
//   NoRemoval — поверхность образуется без удаления слоя материала (черт. 2в).
// Высота короткой черты h равна высоте цифр размерных чисел, высота длинной H — (1,5…5)h (п. 1.3).
// Значение параметра пишется с символом: «Ra 3,2», «Rz 50» (п. 1.5); способ обработки — над полкой.
// Знак без надписей изображается без полки (п. 1.2).
class RoughnessSign : public document::ObjectT<RoughnessSign, Entity> {
public:
    static constexpr std::string_view kType = "Roughness";
    enum class Kind { Any, Removal, NoRemoval } kind = Kind::Any;
    Point2 position;         // вершина знака — точка на линии поверхности
    int quarterTurns = 0;    // поворот знака четвертями оборота против часовой стрелки (только у знака без полки)
    std::string parameter;   // «Ra 3,2»; пусто — без значения
    std::string method;      // способ обработки над полкой; обычно пусто
    double textHeight = 3.5; // h
    bool others = false;     // рядом — такой же знак без надписей в скобках: «остальные поверхности» (п. 2.7)
    double othersHeight = 3.5;

    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::optional<double> textHeightValue() const override { return textHeight; }
    std::vector<Primitive> primitives() const override;
    std::vector<Point2> snapPoints() const override { return {position}; }

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Блок — рисунок, который нарисован один раз и вставляется на чертёж сколько угодно раз.
// Описание блока хранит его графику в собственных координатах (базовая точка — начало координат).
// Имя блока — в Object::name.
class BlockDefinition : public document::ObjectT<BlockDefinition> {
public:
    static constexpr std::string_view kType = "BlockDefinition";
    std::vector<Primitive> content;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Вставка блока: место, поворот и масштаб. Графика копируется из описания при вставке
// и обновляется сервисом, когда блок переопределяют.
class BlockReference : public document::ObjectT<BlockReference, Entity> {
public:
    static constexpr std::string_view kType = "BlockRef";
    Uuid definitionId;
    Point2 position;     // куда попала базовая точка блока
    double angle = 0.0;  // поворот, радианы
    double scale = 1.0;
    std::vector<Primitive> content;  // графика блока в его координатах

    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;
    std::vector<Primitive> primitives() const override;
    std::vector<Point2> snapPoints() const override;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Число знаков в строке UTF-8 (а не байтов) — для оценки ширины текста.
std::size_t characterCount(const std::string& text);

// Штриховка замкнутой области. Контуры — замкнутые ломаные; область определяется правилом
// «чёт-нечет», поэтому вложенный контур (отверстие) остаётся незаштрихованным.
// Штриховка не связана с исходными линиями: после их изменения её нужно построить заново.
class Hatch : public document::ObjectT<Hatch, Entity> {
public:
    static constexpr std::string_view kType = "Hatch";
    std::vector<std::vector<Point2>> loops;
    double angle = 0.7853981633974483;  // наклон линий, радианы (45°)
    double spacing = 3.0;               // расстояние между линиями, мм

    // Отрезки линий штриховки.
    std::vector<std::pair<Point2, Point2>> strokes() const;
    BBox2 bounds() const override;
    void transform(const geometry::Transform2& t) override;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Вид детали на чертеже, построенный по 3D-модели. Хранит, откуда вид взят и какие линии
// чертежа к нему относятся, чтобы вид можно было обновить после изменения модели.
class ModelView : public document::ObjectT<ModelView> {
public:
    static constexpr std::string_view kType = "ModelView";
    Uuid sourceDocumentId;      // документ детали или сборки
    std::string view = "front"; // front, top, left, right, back, bottom
    Point2 origin;              // куда на листе попадает начало координат вида
    double scale = 1.0;
    bool showHidden = false;    // показывать невидимые линии штриховыми
    bool section = false;       // разрез: часть модели перед секущей плоскостью убрана, срез заштрихован
    bool exploded = false;      // вид сборки в разнесённом состоянии
    bool sectionOnly = false;   // сечение: показан только сам срез, без того, что за плоскостью
    std::string stamp;          // отпечаток линий вида: по нему видно, что модель с тех пор изменилась
    double sectionAt = 0.0;     // где проходит секущая плоскость: координата модели вдоль направления взгляда
    std::string label;          // надпись над изображением («А–А»)
    std::vector<Uuid> entities; // линии вида, штриховка и надпись
    std::vector<Uuid> marks;    // разрез: штрихи секущей плоскости, стрелки и буквы на другом виде
    std::vector<Uuid> dimensions; // размеры, поставленные на этот вид (следуют за ним при обновлении)

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Регистрация типов чертежа в реестре — нужна для чтения из файла.
void registerTypes(document::ObjectFactory& factory);

}  // namespace cosmocad::drawing
