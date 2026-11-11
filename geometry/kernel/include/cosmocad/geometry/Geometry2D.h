// Собственные 2D-типы CosmoCad. Не зависят от OCCT и от Qt.
#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace cosmocad::geometry {

inline constexpr double kLinearTolerance = 1e-6;   // мм
inline constexpr double kAngularTolerance = 1e-9;  // рад

struct Point2 {
    double x = 0.0;
    double y = 0.0;

    friend Point2 operator+(Point2 a, Point2 b) { return {a.x + b.x, a.y + b.y}; }
    friend Point2 operator-(Point2 a, Point2 b) { return {a.x - b.x, a.y - b.y}; }
    friend Point2 operator*(Point2 a, double k) { return {a.x * k, a.y * k}; }
    friend bool operator==(const Point2& a, const Point2& b) = default;
};

inline double dot(Point2 a, Point2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Point2 a, Point2 b) { return a.x * b.y - a.y * b.x; }
inline double length(Point2 a) { return std::hypot(a.x, a.y); }
inline double distance(Point2 a, Point2 b) { return length(a - b); }
inline Point2 midpoint(Point2 a, Point2 b) { return {(a.x + b.x) / 2.0, (a.y + b.y) / 2.0}; }
inline bool almostEqual(Point2 a, Point2 b, double tolerance = kLinearTolerance) {
    return distance(a, b) <= tolerance;
}

struct BBox2 {
    Point2 min{0.0, 0.0};
    Point2 max{0.0, 0.0};
    bool valid = false;

    void expand(Point2 p);
    void expand(const BBox2& other);
    double width() const { return max.x - min.x; }
    double height() const { return max.y - min.y; }
    bool contains(Point2 p, double tolerance = kLinearTolerance) const;
    bool contains(const BBox2& other, double tolerance = kLinearTolerance) const;
    static BBox2 fromCorners(Point2 a, Point2 b);
};

// Приведение угла к диапазону [0, 2π).
double normalizeAngle(double angle);

// Лежит ли угол на дуге, идущей против часовой стрелки от start до end.
bool angleOnArc(double angle, double start, double end);

Point2 pointOnCircle(Point2 center, double radius, double angle);

double distanceToSegment(Point2 p, Point2 a, Point2 b);
double distanceToCircle(Point2 p, Point2 center, double radius);
double distanceToArc(Point2 p, Point2 center, double radius, double start, double end);

BBox2 arcBounds(Point2 center, double radius, double start, double end);

// Точка пересечения двух отрезков; пусто, если не пересекаются или параллельны.
std::optional<Point2> segmentIntersection(Point2 a1, Point2 a2, Point2 b1, Point2 b2);

// Точка пересечения двух бесконечных прямых; пусто, если прямые параллельны.
std::optional<Point2> lineIntersection(Point2 a1, Point2 a2, Point2 b1, Point2 b2);

// Параметры t (0 — начало, 1 — конец) точек пересечения отрезка ab с окружностью.
std::vector<double> segmentCircleParams(Point2 a, Point2 b, Point2 center, double radius);

// Ломаная, приближающая дугу от start до end против часовой стрелки
// с отклонением от дуги не больше tolerance. Совпадающие углы — полная окружность.
std::vector<Point2> tessellateArc(Point2 center, double radius, double start, double end, double tolerance = 0.01);

// Ближайшая к p точка отрезка.
Point2 closestPointOnSegment(Point2 p, Point2 a, Point2 b);

// Преобразование подобия: перенос, поворот, равномерный масштаб, зеркальное отражение
// и их сочетания. Сохраняет форму: окружность остаётся окружностью.
struct Transform2 {
    double a = 1.0, b = 0.0;    // поворот и масштаб
    double tx = 0.0, ty = 0.0;  // перенос
    bool mirrored = false;

    static Transform2 translation(Point2 delta);
    static Transform2 rotation(Point2 center, double angle);
    static Transform2 scaling(Point2 center, double factor);
    static Transform2 mirror(Point2 axisA, Point2 axisB);  // отражение относительно прямой AB

    Point2 apply(Point2 p) const;
    double scale() const { return std::hypot(a, b); }
    double angle() const { return std::atan2(b, a); }
};

}  // namespace cosmocad::geometry
