// Сцена — плоский список того, что нужно нарисовать: отрезки, окружности, дуги, текст
// с уже вычисленными толщинами и штрихами. Один и тот же список используют экран,
// печать и экспорт, поэтому чертёж везде выглядит одинаково.
// Модуль не зависит от Qt и от OpenGL.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/geometry/Geometry2D.h"
#include "cosmocad/standards/Engine.h"

namespace cosmocad::graphics {

using geometry::BBox2;
using geometry::Point2;

struct SceneItem {
    enum class Kind { Line, Circle, Arc, Text };
    Kind kind = Kind::Line;

    Point2 a;                 // Line: начало; Circle/Arc: центр; Text: левый нижний угол
    Point2 b;                 // Line: конец
    double radius = 0.0;
    double startAngle = 0.0;  // Arc, радианы, против часовой стрелки
    double endAngle = 0.0;
    std::string text;
    double textHeight = 0.0;  // мм
    bool textCentered = false;  // Text: `a` — середина нижнего края строки
    double textAngle = 0.0;     // Text: поворот, радианы

    double width = 0.5;           // толщина линии, мм
    std::vector<double> pattern;  // штрих/пробел, мм; пусто — сплошная
    std::string color = "#000000";
    std::string layer = "0";
    core::Uuid source;            // объект документа; пустой — оформление листа
};

struct Scene {
    std::vector<SceneItem> items;
    double sheetWidth = 0.0;   // 0 — лист не задан
    double sheetHeight = 0.0;
    BBox2 bounds;              // габарит всего содержимого
};

// Строит сцену по документу. Если передан профиль и в документе есть лист,
// добавляет рамку и основную надпись.
Scene buildScene(const document::Document& document, const standards::Profile* profile);

// Преобразование между миллиметрами чертежа и точками экрана (ось Y экрана вниз).
class ViewTransform2D {
public:
    double scale() const { return scale_; }  // точек экрана на миллиметр

    Point2 toScreen(Point2 world) const;
    Point2 toWorld(Point2 screen) const;

    void pan(double dxScreen, double dyScreen);
    void zoomAt(Point2 screenPoint, double factor);  // точка под курсором остаётся на месте
    void fit(const BBox2& box, double viewWidth, double viewHeight, double marginScreen = 20.0);

private:
    double scale_ = 1.0;
    Point2 offset_{0.0, 0.0};  // экранные координаты начала чертежа
};

// --- Выбор и привязки ---

// Ближайший к точке элемент не дальше tolerance (мм); пусто — ничего не выбрано.
// Элементы скрытых и заблокированных слоёв не выбираются.
std::optional<core::Uuid> hitTest(const document::Document& document, Point2 point, double tolerance);

struct Snap {
    enum class Kind { Endpoint, Midpoint, Center, Intersection, Perpendicular, Nearest };
    Kind kind = Kind::Endpoint;
    Point2 point;
};

// Точка привязки не дальше tolerance (мм) от курсора. Порядок предпочтения:
// характерные точки (конец, середина, центр, пересечение), затем основание перпендикуляра
// из точки from (если она задана), затем ближайшая точка на линии.
std::optional<Snap> findSnap(const document::Document& document, Point2 point, double tolerance,
                             std::optional<Point2> from = std::nullopt);

// Номера элементов, целиком попавших в прямоугольник (выбор рамкой).
std::vector<core::Uuid> entitiesInside(const document::Document& document, const BBox2& box);

}  // namespace cosmocad::graphics
