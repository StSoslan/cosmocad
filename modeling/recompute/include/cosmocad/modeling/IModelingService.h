// IModelingService — параметрические 3D-операции и перестроение модели детали.
// Договорённости (ошибки, владение, потоки, отмена): docs/api/interfaces.md.
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/geometry/IGeometryKernel.h"
#include "cosmocad/modeling/PartObjects.h"

namespace cosmocad::modeling {

inline constexpr int kModelingServiceApiVersion = 2;

enum class FeatureState { UpToDate, Failed, Suppressed };

struct FeatureStatus {
    core::Uuid featureId;
    FeatureState state = FeatureState::UpToDate;
    std::string diagnostic;  // причина сбоя понятным языком
};

struct SketchParams {
    std::string name;
    std::string plane = "XY";
    std::string offset = "0";
    std::string faceName;  // если задано — эскиз лежит на этой плоской грани тела
    std::vector<SketchShape> shapes;  // метки (tag) назначает сервис
    core::Uuid sourceDrawingId{};                // эскиз снят с контура этого чертежа…
    std::vector<core::Uuid> sourceEntities{};    // …с этих его элементов
};

struct ExtrudeParams {
    core::Uuid sketchId;
    std::string distance = "10";
    bool cut = false;
    bool reversed = false;
};

struct RevolveParams {
    core::Uuid sketchId;
    std::string axis = "Y";  // ось эскиза: X или Y
    std::string angle = "360";
    bool cut = false;
};

struct BlendParams {
    std::vector<geometry::EdgeRef> edges;
    std::string size = "1";
    bool chamfer = false;  // иначе скругление
};

// Протяжка: контур эскиза ведётся вдоль траектории. Траектория начинается в середине габарита контура;
// path — следующие её точки, отсчитанные от начала, в осях эскиза (x, y — в плоскости, z — вдоль нормали).
struct SweepParams {
    core::Uuid sketchId;
    std::vector<std::array<std::string, 3>> path;
    std::string bendRadius = "0";  // радиус гиба в изломах траектории
    bool cut = false;
};

// Зеркальное отражение: к телу добавляется его зеркальная копия.
struct MirrorParams {
    std::string plane = "YZ";  // плоскость отражения: XY, XZ или YZ
    std::string offset = "0";  // сдвиг плоскости вдоль её нормали, формула
};

struct RecomputeReport {
    std::vector<FeatureStatus> features;  // в порядке построения
    std::size_t recomputed = 0;  // сколько операций пересчитано
    std::size_t reused = 0;      // сколько взято готовыми: их исходные данные не менялись
    bool cancelled = false;
};

struct PartMass {
    geometry::MassProperties geometry;
    double massGrams = 0.0;
};

class IModelingService {
public:
    virtual ~IModelingService() = default;

    // Каждая изменяющая операция — одна транзакция документа (отменяется целиком).
    // После изменения тело устарело: вызовите recompute.
    virtual core::Result<core::Uuid> addParameter(document::Document& part, const std::string& name,
                                                  const std::string& expression) = 0;
    virtual core::Result<void> setParameter(document::Document& part, const std::string& name,
                                            const std::string& expression) = 0;
    virtual core::Result<void> removeParameter(document::Document& part, const std::string& name) = 0;

    virtual core::Result<core::Uuid> addSketch(document::Document& part, const SketchParams& params) = 0;
    // Заменяет фигуры эскиза. Фигуры с прежними метками сохраняют имена своих граней.
    virtual core::Result<void> updateSketch(document::Document& part, const core::Uuid& sketchId,
                                            const std::vector<SketchShape>& shapes) = 0;

    // Операции добавляются в конец дерева построения.
    virtual core::Result<core::Uuid> addExtrude(document::Document& part, const ExtrudeParams& params) = 0;
    virtual core::Result<core::Uuid> addRevolve(document::Document& part, const RevolveParams& params) = 0;
    virtual core::Result<core::Uuid> addBlend(document::Document& part, const BlendParams& params) = 0;
    virtual core::Result<core::Uuid> addMirror(document::Document& part, const MirrorParams& params) = 0;
    virtual core::Result<core::Uuid> addSweep(document::Document& part, const SweepParams& params) = 0;

    virtual core::Result<void> setFeatureValue(document::Document& part, const core::Uuid& featureId,
                                               const std::string& expression) = 0;
    // Массив копий для выдавливания или выреза. Pattern::Kind::None убирает массив.
    virtual core::Result<void> setPattern(document::Document& part, const core::Uuid& featureId,
                                          const Pattern& pattern) = 0;
    virtual core::Result<void> suppressFeature(document::Document& part, const core::Uuid& featureId,
                                               bool suppressed) = 0;
    virtual core::Result<void> reorderFeature(document::Document& part, const core::Uuid& featureId,
                                              std::size_t newIndex) = 0;
    // Откат дерева построения: тело строится только до этой операции включительно, остальные
    // ждут. Пустой номер снимает откат. Сами операции не меняются.
    virtual core::Result<void> rollTo(document::Document& part, const core::Uuid& featureId) = 0;
    // Удаляет операцию; эскиз, которым пользовалась только она, удаляется вместе с ней.
    virtual core::Result<void> removeFeature(document::Document& part, const core::Uuid& featureId) = 0;

    // Исполнения: именованные наборы значений параметров одной детали.
    // saveConfiguration запоминает нынешние формулы всех параметров под этим именем (повтор имени — замена);
    // designation — обозначение исполнения, пусто — обозначение детали при применении не меняется.
    virtual core::Result<void> saveConfiguration(document::Document& part, const std::string& name,
                                                 const std::string& designation = {}) = 0;
    // Записывает формулы исполнения в параметры детали одним шагом отмены. После этого нужен recompute.
    virtual core::Result<void> applyConfiguration(document::Document& part, const std::string& name) = 0;
    virtual core::Result<void> removeConfiguration(document::Document& part, const std::string& name) = 0;
    virtual std::vector<std::string> configurations(const document::Document& part) const = 0;

    virtual core::Result<void> setPartProperties(document::Document& part, const std::string& designation,
                                                 const std::string& material, double density) = 0;

    // Операции в порядке построения.
    virtual std::vector<const Feature*> features(const document::Document& part) const = 0;
    // Значения всех параметров. Ошибка InvalidState — параметры ссылаются друг на друга по кругу.
    virtual core::Result<std::map<std::string, double>> parameterValues(const document::Document& part) const = 0;

    // Пересчитывает только операции, исходные данные которых изменились.
    // Одинаковые исходные данные дают одинаковое тело и одинаковые имена граней.
    // Операция со сбоем не останавливает остальные: тело проходит мимо неё без изменений.
    virtual core::Result<RecomputeReport> recompute(const document::Document& part,
                                                    const std::atomic<bool>* cancel = nullptr) = 0;

    // Результат последнего recompute. Пусто, если тела нет.
    virtual geometry::ShapePtr body(const document::Document& part) const = 0;
    virtual std::vector<FeatureStatus> status(const document::Document& part) const = 0;
    // Плоскость, в которой лежит эскиз на этой грани тела: начало координат эскиза и его оси.
    // Нужна окну, чтобы перевести точку щелчка на грани в координаты эскиза.
    virtual core::Result<geometry::Plane> facePlane(const document::Document& part,
                                                    const std::string& faceName) const = 0;
    virtual core::Result<PartMass> mass(const document::Document& part) const = 0;

    // Забыть рассчитанное тело документа (при закрытии проекта).
    virtual void forget(const core::Uuid& documentId) = 0;
};

}  // namespace cosmocad::modeling
