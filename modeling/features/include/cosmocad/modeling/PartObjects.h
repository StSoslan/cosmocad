// Объекты документа «Деталь»: параметры, эскизы, операции построения, свойства детали.
// Само тело в документе не хранится: оно каждый раз строится заново по этим данным.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cosmocad/document/Object.h"
#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::modeling {

using document::Result;

// Именованное число, на которое ссылаются формулы: «ширина = 80», «шаг = ширина / 4».
class Parameter final : public document::ObjectT<Parameter> {
public:
    static constexpr const char* kType = "Parameter";
    std::string expression = "0";  // имя параметра хранится в Object::name
    std::string comment;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Фигура эскиза. Каждый размер — формула.
//   Rectangle: x, y, ширина, высота (x, y — левый нижний угол)
//   Circle:    x центра, y центра, радиус
//   Polygon:   x1, y1, x2, y2, … (замкнутая ломаная, не меньше трёх точек)
struct SketchShape {
    enum class Kind { Rectangle, Circle, Polygon } kind = Kind::Rectangle;
    std::uint64_t tag = 0;            // постоянный номер фигуры в эскизе: основа имён граней
    bool hole = false;                // отверстие в фигуре, внутри которой лежит
    std::vector<std::string> values;  // формулы
};

class Sketch final : public document::ObjectT<Sketch> {
public:
    static constexpr const char* kType = "Sketch";
    std::string plane = "XY";  // XY, XZ, YZ или пусто, если эскиз лежит на грани
    std::string offset = "0";  // сдвиг плоскости вдоль её нормали, формула
    std::string faceName;      // имя плоской грани тела, на которой лежит эскиз
    std::vector<SketchShape> shapes;
    std::uint64_t nextTag = 1;
    // Эскиз снят с контура чертежа: с какого документа и с каких элементов. Приложение по этой записи
    // обновляет эскиз, когда контур на чертеже изменился. Пусто — эскиз задан своими фигурами.
    core::Uuid sourceDrawingId;
    std::vector<core::Uuid> sourceEntities;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Массив: операция выполняется несколько раз — в ряд или по кругу. Все величины — формулы.
// Положение задаётся в координатах эскиза операции.
struct Pattern {
    enum class Kind { None, Linear, Circular } kind = Kind::None;
    std::string count = "1";     // число копий вместе с исходной
    std::string stepX = "0";     // в ряд: шаг между копиями по X и по Y эскиза
    std::string stepY = "0";
    std::string centerX = "0";   // по кругу: центр круга
    std::string centerY = "0";
    std::string angle = "360";   // по кругу: на каком угле разложить копии; 360 — равномерно по всему кругу
};

enum class FeatureKind { Extrude, Revolve, Fillet, Chamfer, Mirror, Sweep };
const char* toString(FeatureKind kind);

// Операция построения. Порядок операций — по полю sequence.
class Feature final : public document::ObjectT<Feature> {
public:
    static constexpr const char* kType = "Feature";
    FeatureKind kind = FeatureKind::Extrude;
    std::string tag;            // короткая метка («E1», «F2») — начало имён граней этой операции
    std::int64_t sequence = 0;
    bool suppressed = false;

    core::Uuid sketchId;        // выдавливание, вращение
    std::string value = "10";   // расстояние, угол, радиус скругления, размер фаски — формула
    bool cut = false;           // вычесть из тела
    bool reversed = false;      // выдавливать в обратную сторону
    std::string axis = "Y";     // вращение: ось эскиза, X или Y; зеркало: плоскость XY, XZ или YZ
                                // (у зеркала value — сдвиг плоскости вдоль её нормали)
    std::vector<geometry::EdgeRef> edges;  // скругление, фаска
    Pattern pattern;            // выдавливание и вырез: массив копий
    // Протяжка: точки траектории после начальной, формулы. Координаты — в осях эскиза: x, y — в его
    // плоскости, z — вдоль нормали; отсчёт — от начала траектории, а оно лежит в середине габарита контура.
    // value — радиус гиба.
    std::vector<std::array<std::string, 3>> path;

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Сведения о детали для основной надписи и спецификации.
class PartProperties final : public document::ObjectT<PartProperties> {
public:
    static constexpr const char* kType = "PartProperties";
    std::string designation;  // обозначение
    std::string material;
    double density = 7.85;    // г/см³ (по умолчанию сталь)
    core::Uuid rollbackAfter; // откат дерева: построение останавливается после этой операции; пусто — отката нет

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

// Исполнение детали: именованный набор значений параметров (имя — в Object::name).
// Применить исполнение — значит записать эти формулы в параметры детали.
class Configuration final : public document::ObjectT<Configuration> {
public:
    static constexpr const char* kType = "Configuration";
    std::map<std::string, std::string> values;  // имя параметра → формула
    std::string designation;                    // обозначение исполнения («КСМ.003.001-01»); пусто — не менять

protected:
    void writeJson(nlohmann::json& json) const override;
    Result<void> readJson(const nlohmann::json& json) override;
};

void registerTypes(document::ObjectFactory& factory);

}  // namespace cosmocad::modeling
