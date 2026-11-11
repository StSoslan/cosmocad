// IDrawingService — листы, 2D-элементы, размеры и оформление чертежа.
// Каждая операция — отдельная транзакция: её можно отменить одним шагом.
// Виды по 3D-модели (этап 7 плана) в версии 0.1 отсутствуют.
// Договорённости: docs/api/interfaces.md.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cosmocad/document/Document.h"
#include "cosmocad/drawing/Entities.h"
#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::drawing {

inline constexpr int kDrawingServiceApiVersion = 1;

// Правка свойств уже созданного элемента. Заполняются только изменяемые поля.
// Поле, которого у элемента нет (например, радиус у текста), — ошибка, а не молчаливый пропуск.
struct EntityEdit {
    std::optional<std::string> text;            // текст, выноска
    std::optional<double> textHeight;           // текст, выноска, размеры
    std::optional<double> radius;               // окружность, дуга
    std::optional<double> offset;               // линейный размер: отступ размерной линии
    std::optional<bool> diameter;               // размер радиуса ↔ диаметра
    std::optional<double> hatchSpacing;         // штриховка
    std::optional<double> hatchAngleDegrees;    // штриховка
    std::optional<DimensionNote> note;          // линейный размер, размер радиуса или диаметра
};

struct SheetSetup {
    std::string profileId;       // профиль стандарта
    std::string profileVersion;
    std::string format = "A4";
    bool landscape = false;
    double mainLineWidth = 0.8;
};

// Где на другом виде показать секущую плоскость разреза (ГОСТ 2.305-2008): концы следа плоскости
// сразу за контуром этого вида и направление взгляда. Координаты листа.
struct SectionMark {
    Point2 a, b;
    Point2 direction;  // единичный вектор
};

struct RoughnessParams {
    Point2 position;                 // вершина знака на линии поверхности, выносной линии или полке выноски
    RoughnessSign::Kind kind = RoughnessSign::Kind::Any;
    std::string parameter;           // «Ra 3,2», «Rz 50»; пусто — знак без значения
    std::string method;              // способ обработки, если он единственно возможный
    int quarterTurns = 0;            // поворот знака без надписей
    double textHeight = 3.5;
    Uuid layer;
};

// Проверяет запись значения шероховатости: «Ra 3,2», «Rz 50»… Пустая строка допустима.
Result<void> checkRoughnessParameter(const std::string& text);

struct ModelViewParams {
    Uuid sourceDocumentId;
    std::string view = "front";
    Point2 center;       // где на листе должна оказаться середина вида
    double scale = 1.0;
    bool showHidden = false;
    bool section = false;     // разрез (в проекции должны быть sectionAreas)
    double sectionAt = 0.0;
    bool sectionOnly = false;  // сечение
    bool exploded = false;     // разнесённый вид сборки
    std::string label;        // надпись над изображением
    std::optional<SectionMark> mark;  // обозначение секущей плоскости на другом виде
};

// Отпечаток линий вида: одинаковые линии дают одинаковую строку. По нему определяется, устарел ли вид.
std::string viewStamp(const geometry::ProjectedView& projected);

// Что стало с размерами при обновлении вида по модели.
struct ModelViewUpdate {
    int movedDimensions = 0;  // размеры, переехавшие вместе с линиями вида
    int lostDimensions = 0;   // размеры, для которых новое место определить не удалось
};

class IDrawingService {
public:
    virtual ~IDrawingService() = default;

    // Готовит документ-чертёж: выбирает профиль и создаёт лист.
    virtual Result<Uuid> setupSheet(document::Document& document, const SheetSetup& setup) = 0;
    virtual Result<void> setTitleBlockField(document::Document& document, const std::string& field,
                                            const std::string& value) = 0;

    virtual Result<Uuid> addLayer(document::Document& document, const std::string& name,
                                  const std::string& lineTypeId, const std::string& color = "#000000") = 0;

    virtual Result<Uuid> addLine(document::Document& document, Point2 start, Point2 end, const Uuid& layer = {}) = 0;
    virtual Result<Uuid> addCircle(document::Document& document, Point2 center, double radius,
                                   const Uuid& layer = {}) = 0;
    virtual Result<Uuid> addArc(document::Document& document, Point2 center, double radius, double startAngle,
                                double endAngle, const Uuid& layer = {}) = 0;
    virtual Result<Uuid> addText(document::Document& document, Point2 position, const std::string& text,
                                 double height, const Uuid& layer = {}) = 0;
    virtual Result<Uuid> addLinearDimension(document::Document& document, Point2 p1, Point2 p2, double offset,
                                            const Uuid& layer = {}) = 0;

    // Размер радиуса (diameter = false) или диаметра. angle — куда направлена стрелка.
    virtual Result<Uuid> addRadialDimension(document::Document& document, Point2 center, double radius,
                                            double angle, bool diameter, const Uuid& layer = {}) = 0;
    // Угловой размер: от луча vertex→p1 до луча vertex→p2 против часовой стрелки.
    virtual Result<Uuid> addAngularDimension(document::Document& document, Point2 vertex, Point2 p1, Point2 p2,
                                             double arcRadius, const Uuid& layer = {}) = 0;

    // Линия-выноска со стрелкой в tip, изломом в knee и текстом на полке.
    // Знак шероховатости (ГОСТ 2.309-73). Значение параметра должно начинаться с его символа.
    // Знак с надписью ставится только прямо (на горизонтальную линию или полку выноски).
    // Блоки. createBlock собирает блок из выбранных элементов (линии, окружности, дуги, тексты, штриховка,
    // составные объекты и другие блоки; размеры и выноски в блок не берутся). basePoint — точка, за которую
    // блок потом ставится. replace — заменить исходные элементы вставкой блока на том же месте.
    // Блок с таким именем уже есть — он переопределяется, и все его вставки обновляются.
    virtual Result<Uuid> createBlock(document::Document& document, const std::string& name, const std::vector<Uuid>& ids,
                                     Point2 basePoint, bool replace) = 0;
    virtual Result<Uuid> insertBlock(document::Document& document, const std::string& name, Point2 position,
                                     double angleDegrees = 0.0, double scale = 1.0, const Uuid& layer = {}) = 0;
    // Разбивает вставку блока на обычные элементы чертежа; возвращает их номера.
    virtual Result<std::vector<Uuid>> explodeBlock(document::Document& document, const Uuid& referenceId) = 0;
    virtual std::vector<std::string> blockNames(const document::Document& document) const = 0;

    virtual Result<Uuid> addRoughness(document::Document& document, const RoughnessParams& params) = 0;
    virtual Result<Uuid> addLeader(document::Document& document, Point2 tip, Point2 knee, const std::string& text,
                                   double textHeight, const Uuid& layer = {}) = 0;

    // Изменение свойств существующего элемента — один шаг отмены.
    virtual Result<void> editEntity(document::Document& document, const Uuid& id, const EntityEdit& edit) = 0;

    // Штриховка области, ограниченной перечисленными отрезками, дугами и окружностями.
    // Отрезки и дуги должны смыкаться концами в замкнутые контуры; окружность — контур сама по себе.
    // Контур внутри контура остаётся незаштрихованным (отверстие).
    virtual Result<Uuid> addHatch(document::Document& document, const std::vector<Uuid>& boundary, double angle,
                                  double spacing, const Uuid& layer = {}) = 0;

    // Обрезка отрезка: удаляет его часть вокруг точки pick до ближайших пересечений
    // с другими отрезками, окружностями и дугами.
    virtual Result<void> trimLine(document::Document& document, const Uuid& line, Point2 pick) = 0;

    // Скругление угла между двумя отрезками. pickA и pickB показывают, какие стороны оставить.
    // Радиус 0 — просто довести отрезки до острого угла.
    virtual Result<void> filletLines(document::Document& document, const Uuid& lineA, Point2 pickA,
                                     const Uuid& lineB, Point2 pickB, double radius) = 0;
    // Фаска: срез угла на расстоянии distance от вершины по каждому отрезку.
    virtual Result<void> chamferLines(document::Document& document, const Uuid& lineA, Point2 pickA,
                                      const Uuid& lineB, Point2 pickB, double distance) = 0;

    // Прямоугольник со сторонами вдоль осей: четыре отрезка одной транзакцией.
    virtual Result<std::vector<Uuid>> addRectangle(document::Document& document, Point2 corner1, Point2 corner2,
                                                   const Uuid& layer = {}) = 0;

    virtual Result<void> moveEntities(document::Document& document, const std::vector<Uuid>& ids, Point2 delta) = 0;
    // Перенос, поворот, масштаб или отражение выбранных элементов — один шаг отмены.
    virtual Result<void> transformEntities(document::Document& document, const std::vector<Uuid>& ids,
                                           const geometry::Transform2& transform, const std::string& name) = 0;
    // Копии выбранных элементов, преобразованные заданным образом. Возвращает номера копий.
    virtual Result<std::vector<Uuid>> copyEntities(document::Document& document, const std::vector<Uuid>& ids,
                                                   const geometry::Transform2& transform) = 0;
    virtual Result<void> setEntitiesLayer(document::Document& document, const std::vector<Uuid>& ids,
                                          const Uuid& layer) = 0;
    virtual Result<void> removeObjects(document::Document& document, const std::vector<Uuid>& ids) = 0;

    virtual Result<void> setLayerState(document::Document& document, const Uuid& layer, bool visible,
                                       bool locked) = 0;
    // Меняет формат, ориентацию, толщину основной линии и поля основной надписи одним шагом.
    virtual Result<void> updateSheet(document::Document& document, const std::string& format, bool landscape,
                                     double mainLineWidth,
                                     const std::map<std::string, std::string>& titleBlock) = 0;

    // Вид по 3D-модели. projected — линии вида, рассчитанные геометрическим ядром.
    // Видимые линии — основные, невидимые — штриховые. Одна транзакция.
    virtual Result<Uuid> addModelView(document::Document& document, const ModelViewParams& params,
                                      const geometry::ProjectedView& projected) = 0;
    // Заменяет линии вида новыми (после изменения модели). Место вида на листе сохраняется.
    // Размеры, поставленные на точки вида (концы линий, центры окружностей, углы габарита), переезжают следом:
    // точка находит своё новое место по порядку среди характерных точек вида. Если после
    // изменения модели число таких точек изменилось, размер остаётся на месте и получает
    // имя «Размер: привязка потеряна».
    // mark — новое место обозначения секущей плоскости (для разреза); пусто — обозначения нет.
    virtual Result<ModelViewUpdate> updateModelView(document::Document& document, const Uuid& viewId,
                                                    const geometry::ProjectedView& projected,
                                                    const std::optional<SectionMark>& mark = std::nullopt,
                                                    std::optional<double> newSectionAt = std::nullopt) = 0;
};

class DrawingService final : public IDrawingService {
public:
    Result<Uuid> setupSheet(document::Document& document, const SheetSetup& setup) override;
    Result<void> setTitleBlockField(document::Document& document, const std::string& field,
                                    const std::string& value) override;
    Result<Uuid> addLayer(document::Document& document, const std::string& name, const std::string& lineTypeId,
                          const std::string& color) override;
    Result<Uuid> addLine(document::Document& document, Point2 start, Point2 end, const Uuid& layer) override;
    Result<Uuid> addCircle(document::Document& document, Point2 center, double radius, const Uuid& layer) override;
    Result<Uuid> addArc(document::Document& document, Point2 center, double radius, double startAngle,
                        double endAngle, const Uuid& layer) override;
    Result<Uuid> addText(document::Document& document, Point2 position, const std::string& text, double height,
                         const Uuid& layer) override;
    Result<Uuid> addLinearDimension(document::Document& document, Point2 p1, Point2 p2, double offset,
                                    const Uuid& layer) override;
    Result<Uuid> addRadialDimension(document::Document& document, Point2 center, double radius, double angle,
                                    bool diameter, const Uuid& layer) override;
    Result<Uuid> addAngularDimension(document::Document& document, Point2 vertex, Point2 p1, Point2 p2,
                                     double arcRadius, const Uuid& layer) override;
    Result<Uuid> addHatch(document::Document& document, const std::vector<Uuid>& boundary, double angle,
                          double spacing, const Uuid& layer) override;
    Result<Uuid> createBlock(document::Document& document, const std::string& name, const std::vector<Uuid>& ids,
                             Point2 basePoint, bool replace) override;
    Result<Uuid> insertBlock(document::Document& document, const std::string& name, Point2 position,
                             double angleDegrees = 0.0, double scale = 1.0, const Uuid& layer = {}) override;
    Result<std::vector<Uuid>> explodeBlock(document::Document& document, const Uuid& referenceId) override;
    std::vector<std::string> blockNames(const document::Document& document) const override;
    Result<Uuid> addRoughness(document::Document& document, const RoughnessParams& params) override;
    Result<Uuid> addLeader(document::Document& document, Point2 tip, Point2 knee, const std::string& text,
                           double textHeight, const Uuid& layer) override;
    Result<void> editEntity(document::Document& document, const Uuid& id, const EntityEdit& edit) override;
    Result<void> trimLine(document::Document& document, const Uuid& line, Point2 pick) override;
    Result<void> filletLines(document::Document& document, const Uuid& lineA, Point2 pickA, const Uuid& lineB,
                             Point2 pickB, double radius) override;
    Result<void> chamferLines(document::Document& document, const Uuid& lineA, Point2 pickA, const Uuid& lineB,
                              Point2 pickB, double distance) override;
    Result<std::vector<Uuid>> addRectangle(document::Document& document, Point2 corner1, Point2 corner2,
                                           const Uuid& layer) override;

private:
    Result<void> cornerLines(document::Document& document, const Uuid& lineA, Point2 pickA, const Uuid& lineB,
                             Point2 pickB, double size, bool chamfer);

public:
    Result<void> moveEntities(document::Document& document, const std::vector<Uuid>& ids, Point2 delta) override;
    Result<void> transformEntities(document::Document& document, const std::vector<Uuid>& ids,
                                   const geometry::Transform2& transform, const std::string& name) override;
    Result<std::vector<Uuid>> copyEntities(document::Document& document, const std::vector<Uuid>& ids,
                                           const geometry::Transform2& transform) override;
    Result<void> setEntitiesLayer(document::Document& document, const std::vector<Uuid>& ids,
                                  const Uuid& layer) override;
    Result<void> removeObjects(document::Document& document, const std::vector<Uuid>& ids) override;
    Result<void> setLayerState(document::Document& document, const Uuid& layer, bool visible, bool locked) override;
    Result<void> updateSheet(document::Document& document, const std::string& format, bool landscape,
                             double mainLineWidth, const std::map<std::string, std::string>& titleBlock) override;
    Result<Uuid> addModelView(document::Document& document, const ModelViewParams& params,
                              const geometry::ProjectedView& projected) override;
    Result<ModelViewUpdate> updateModelView(document::Document& document, const Uuid& viewId,
                                            const geometry::ProjectedView& projected,
                                            const std::optional<SectionMark>& mark = std::nullopt,
                                            std::optional<double> newSectionAt = std::nullopt) override;
};

}  // namespace cosmocad::drawing
