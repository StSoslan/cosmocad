// IGeometryKernel — граница между CosmoCad и 3D-геометрическим ядром (OCCT).
// Остальные модули видят только этот интерфейс и собственные типы CosmoCad,
// поэтому ядро можно заменить, не трогая интерфейс пользователя и модель данных.
//
// Реализация: адаптер OCCT в geometry/adapters/occt.
// Договорённости (ошибки, владение, потоки, отмена): docs/api/interfaces.md.
//
// УСТОЙЧИВЫЕ ИМЕНА ГРАНЕЙ (требование 3D-11). Каждая грань тела имеет имя, которое
// зависит от того, КАК грань получена, а не от её порядкового номера в теле:
//   "<метка операции>/side/<метка элемента эскиза>" — боковая грань выдавливания или вращения;
//   "<метка операции>/start", "<метка операции>/end" — торцы;
//   "<метка операции>/fillet/<номер ребра в списке>" — грань скругления или фаски.
// Булевы операции, скругления и фаски сохраняют имена граней, которые они изменили.
// Если операция разрезала грань на несколько частей, части получают имена
// "имя", "имя#2", "имя#3" в порядке координат центров частей.
// Ребро задаётся парой имён граней, между которыми оно лежит.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/geometry/Geometry2D.h"

namespace cosmocad::geometry {

inline constexpr int kGeometryKernelApiVersion = 4;

struct Point3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct Vector3 {
    double x = 0.0, y = 0.0, z = 1.0;
};
struct Plane {
    Point3 origin;
    Vector3 normal;
    Vector3 xDirection{1.0, 0.0, 0.0};
};

// Положение в пространстве: матрица 3×4 (поворот и перенос), по строкам.
struct Placement {
    std::array<double, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    Point3 apply(const Point3& p) const;
    Vector3 rotate(const Vector3& v) const;
    // Сначала применяется inner, потом this.
    Placement operator*(const Placement& inner) const;
    static Placement translation(double dx, double dy, double dz);
    // Поворот вокруг оси, проходящей через точку, на угол в радианах.
    static Placement rotation(const Point3& point, const Vector3& axis, double angle);
};

Vector3 normalized(const Vector3& v);
Vector3 crossProduct(const Vector3& a, const Vector3& b);
double dotProduct(const Vector3& a, const Vector3& b);

// Непрозрачная ссылка на тело внутри ядра. Тело неизменяемо: каждая операция
// возвращает новое тело. Время жизни управляется подсчётом ссылок.
class IShape {
public:
    virtual ~IShape() = default;
};
using ShapePtr = std::shared_ptr<const IShape>;

// Замкнутый плоский контур — результат эскиза.
struct ProfileSegment {
    enum class Kind { Line, Arc, Circle } kind = Kind::Line;
    Point2 start, end;       // для окружности start — любая точка на ней
    Point2 center;           // для дуги и окружности
    bool clockwise = false;  // для дуги
    std::uint64_t sourceTag = 0;  // метка элемента эскиза — основа устойчивых имён граней
};
struct Profile2D {
    std::vector<ProfileSegment> outer;
    std::vector<std::vector<ProfileSegment>> holes;
};

enum class BooleanKind { Union, Subtract, Intersect };

// Ребро — линия между двумя гранями с такими именами.
struct EdgeRef {
    std::string faceA, faceB;
};

struct FaceInfo {
    enum class Kind { Plane, Cylinder, Other } kind = Kind::Other;
    Point3 point;       // плоскость: точка на ней; цилиндр: точка на оси
    Vector3 direction;  // плоскость: нормаль наружу тела; цилиндр: направление оси
    double radius = 0.0;
    double area = 0.0;
    Point3 centroid;
};

struct MassProperties {
    double volume = 0.0;        // мм³
    double surfaceArea = 0.0;   // мм²
    Point3 centerOfMass;
};

struct Mesh {
    std::vector<Point3> vertices;
    std::vector<Vector3> normals;  // по одной на вершину
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::uint32_t> triangleFace;  // номер грани в faceNames для каждого треугольника
    std::vector<std::string> faceNames;
    std::vector<std::vector<Point3>> edges;  // рёбра тела ломаными — для показа контура
};

// Линия вида на чертеже (проекция тела с удалением невидимых линий).
struct ProjectedCurve {
    enum class Kind { Line, Circle, Arc } kind = Kind::Line;
    Point2 a, b;  // отрезок
    Point2 center;
    double radius = 0.0;
    double startAngle = 0.0, endAngle = 0.0;  // дуга против часовой стрелки
    bool hidden = false;
};
// Место, где секущая плоскость разреза прошла через материал одного тела: наружный контур
// и контуры отверстий, замкнутые ломаные в координатах вида.
struct SectionArea {
    std::vector<std::vector<Point2>> loops;
    int body = 0;  // номер тела по порядку: соседние детали сборки штрихуются в разные стороны
};
struct ProjectedView {
    std::vector<ProjectedCurve> curves;
    std::vector<SectionArea> sectionAreas;  // только у разреза
};

struct Bounds3 {
    Point3 min, max;
};

// Флаг отмены длительной операции: ядро проверяет его между шагами
// и возвращает ошибку Cancelled.
using CancelFlag = std::atomic<bool>;

class IGeometryKernel {
public:
    virtual ~IGeometryKernel() = default;

    virtual int apiVersion() const = 0;

    // distance может быть отрицательным — тогда выдавливание идёт против нормали плоскости.
    virtual core::Result<ShapePtr> extrude(const Profile2D& profile, const Plane& plane, double distance,
                                           const std::string& tag, const CancelFlag* cancel = nullptr) = 0;
    // Угол в градусах, от 0 до 360.
    virtual core::Result<ShapePtr> revolve(const Profile2D& profile, const Plane& plane, const Point3& axisPoint,
                                           const Vector3& axisDirection, double angleDegrees,
                                           const std::string& tag, const CancelFlag* cancel = nullptr) = 0;
    // Протяжка: контур ведётся вдоль траектории — ломаной из точек path (первая точка лежит в плоскости
    // контура). Изломы скругляются радиусом bendRadius; у траектории с изломами он должен быть больше нуля.
    // Имена граней: «<метка>/new/<k>» в порядке координат.
    virtual core::Result<ShapePtr> sweep(const Profile2D& profile, const Plane& plane, const std::vector<Point3>& path,
                                         double bendRadius, const std::string& tag,
                                         const CancelFlag* cancel = nullptr) = 0;
    virtual core::Result<ShapePtr> boolean(BooleanKind kind, const ShapePtr& a, const ShapePtr& b,
                                           const CancelFlag* cancel = nullptr) = 0;
    virtual core::Result<ShapePtr> fillet(const ShapePtr& shape, const std::vector<EdgeRef>& edges, double radius,
                                          const std::string& tag) = 0;
    virtual core::Result<ShapePtr> chamfer(const ShapePtr& shape, const std::vector<EdgeRef>& edges,
                                           double distance, const std::string& tag) = 0;
    virtual core::Result<ShapePtr> transformed(const ShapePtr& shape, const Placement& placement) = 0;
    // Зеркальная копия тела относительно плоскости. Имена граней копии: «<метка>/mirror/<прежнее имя>».
    virtual core::Result<ShapePtr> mirrored(const ShapePtr& shape, const Point3& planePoint,
                                            const Vector3& planeNormal, const std::string& tag) = 0;

    // Несколько тел как одно целое (для вида сборки и экспорта). Тела не сливаются;
    // имена граней получают приставку «<номер тела>:».
    virtual core::Result<ShapePtr> combine(const std::vector<ShapePtr>& shapes) = 0;

    virtual std::vector<std::string> faceNames(const ShapePtr& shape) const = 0;
    virtual core::Result<FaceInfo> faceInfo(const ShapePtr& shape, const std::string& faceName) const = 0;
    // Есть ли в теле ребро между этими гранями.
    virtual bool hasEdge(const ShapePtr& shape, const EdgeRef& edge) const = 0;

    // Проверка корректности тела. Некорректное тело в документ не попадает (требование КАЧ-4).
    virtual core::Result<void> validate(const ShapePtr& shape) = 0;
    virtual core::Result<MassProperties> massProperties(const ShapePtr& shape) = 0;
    virtual core::Result<Mesh> tessellate(const ShapePtr& shape, double linearDeflection,
                                          const CancelFlag* cancel = nullptr) = 0;
    // Объём общей части двух тел, мм³ (проверка пересечений в сборке).
    virtual core::Result<double> commonVolume(const ShapePtr& a, const ShapePtr& b) = 0;

    // Наименьшее расстояние между двумя телами, мм (зазор в сборке); 0 — тела касаются или пересекаются.
    virtual core::Result<double> distance(const ShapePtr& a, const ShapePtr& b) = 0;

    // Вид для чертежа. viewDirection — куда смотрит наблюдатель, up — что на виде «вверх».
    virtual core::Result<ProjectedView> project(const ShapePtr& shape, const Vector3& viewDirection,
                                                const Vector3& up, bool withHidden) = 0;

    // Разрез для чертежа: часть тела перед секущей плоскостью (со стороны наблюдателя) убирается,
    // остаток проецируется как обычный вид. Плоскость перпендикулярна направлению взгляда и
    // проходит через planePoint. В sectionAreas — места, где плоскость прошла через материал.
    // whole — тела, которые показываются нерассечёнными (валы, крепёж); может быть пустым.
    // onlyCut — сечение: показывается только то, что лежит в самой секущей плоскости.
    virtual core::Result<ProjectedView> projectSection(const ShapePtr& shape, const Vector3& viewDirection,
                                                       const Vector3& up, const Point3& planePoint,
                                                       bool withHidden, const ShapePtr& whole,
                                                       bool onlyCut) = 0;
    // Габарит тела вдоль осей координат.
    virtual core::Result<Bounds3> bounds(const ShapePtr& shape) = 0;

    virtual core::Result<void> exportStep(const std::vector<ShapePtr>& shapes,
                                          const std::filesystem::path& path) = 0;
    virtual core::Result<ShapePtr> importStep(const std::filesystem::path& path, const std::string& tag) = 0;
    virtual core::Result<void> exportStl(const ShapePtr& shape, const std::filesystem::path& path,
                                         double linearDeflection) = 0;
};

}  // namespace cosmocad::geometry
