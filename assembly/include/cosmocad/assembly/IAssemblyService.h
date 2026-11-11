// IAssemblyService — компоненты сборки, сопряжения, проверка пересечений, состав изделия.
// Договорённости: docs/api/interfaces.md.
//
// Сборка хранит не копии деталей, а ссылки на документы деталей и положение каждого
// компонента. Тела деталей сборка получает через IPartSource, поэтому модуль сборок
// не зависит от модуля моделирования.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::assembly {

inline constexpr int kAssemblyServiceApiVersion = 3;

using geometry::Placement;

enum class MateKind { Coincident, Concentric, Parallel, Distance, Angle, Tangent };
const char* toString(MateKind kind);

// Ссылка на грань компонента по устойчивому имени (см. требование 3D-11).
struct GeometryRef {
    core::Uuid componentId;
    std::string faceName;
};

// Компонент сборки. Имя — в Object::name.
class Component final : public document::ObjectT<Component> {
public:
    static constexpr const char* kType = "Component";
    core::Uuid sourceDocumentId;
    Placement placement;
    bool fixed = false;  // закреплён: сопряжения его не двигают
    // На разрезе чертежа показывается нерассечённым (валы, болты, шпонки — ГОСТ 2.305-2008).
    bool wholeInSection = false;
    // Разнесённый вид: на сколько компонент отодвинут от своего места (в координатах сборки).
    geometry::Vector3 explode{0.0, 0.0, 0.0};

protected:
    void writeJson(nlohmann::json& json) const override;
    document::Result<void> readJson(const nlohmann::json& json) override;
};

// Сопряжение двух граней.
//   Coincident — плоские грани совмещены и смотрят навстречу (с flip — в одну сторону);
//   Distance   — плоские грани параллельны, вторая на расстоянии value от первой;
//   Parallel   — плоские грани параллельны;
//   Concentric — оси цилиндрических граней совпадают;
//   Angle      — плоские грани под углом value (градусы) между направлениями «наружу»;
//   Tangent    — цилиндрическая грань касается плоской (одна грань — плоскость, другая — цилиндр).
class Mate final : public document::ObjectT<Mate> {
public:
    static constexpr const char* kType = "Mate";
    MateKind kind = MateKind::Coincident;
    GeometryRef first, second;
    double value = 0.0;  // расстояние, мм; для угла — градусы
    bool flip = false;

protected:
    void writeJson(nlohmann::json& json) const override;
    document::Result<void> readJson(const nlohmann::json& json) override;
};

void registerTypes(document::ObjectFactory& factory);

struct MateParams {
    MateKind kind = MateKind::Coincident;
    GeometryRef first, second;
    double value = 0.0;
    bool flip = false;
};

struct SolveReport {
    bool solved = false;
    int passes = 0;
    // Сопряжения, которые не удалось выполнить, с объяснением.
    std::vector<std::pair<core::Uuid, std::string>> conflicts;
};

struct Interference {
    core::Uuid first, second;
    double volume = 0.0;  // мм³
};

struct BomLine {
    core::Uuid documentId;
    std::string designation;
    std::string name;
    std::string material;
    int quantity = 0;
    double massGrams = 0.0;  // одной штуки
};

// Сведения о детали или подсборке, на которую ссылается компонент.
struct PartInfo {
    std::string designation;
    std::string name;
    std::string material;
    double massGrams = 0.0;
};

// Откуда сборка берёт тела и сведения о деталях. Реализуется приложением.
class IPartSource {
public:
    virtual ~IPartSource() = default;
    virtual core::Result<geometry::ShapePtr> bodyOf(const core::Uuid& documentId) = 0;
    virtual core::Result<PartInfo> infoOf(const core::Uuid& documentId) = 0;
};

// Сколько независимых движений осталось у компонента при нынешних сопряжениях:
// 0 — положение определено полностью, 6 — компонент ничем не удерживается.
struct Freedom {
    core::Uuid componentId;
    int free = 0;
    bool fixed = false;  // закреплён напрямую
};

struct PlacedBody {
    core::Uuid componentId;
    geometry::ShapePtr shape;  // тело в координатах сборки
    bool wholeInSection = false;
};

class IAssemblyService {
public:
    virtual ~IAssemblyService() = default;

    // Первый компонент сборки закрепляется автоматически.
    virtual core::Result<core::Uuid> addComponent(document::Document& assembly, const core::Uuid& sourceDocumentId,
                                                  const Placement& placement, const std::string& name = {}) = 0;
    // Удаляет компонент и сопряжения, которые на него ссылаются.
    virtual core::Result<void> removeComponent(document::Document& assembly, const core::Uuid& componentId) = 0;
    virtual core::Result<void> setPlacement(document::Document& assembly, const core::Uuid& componentId,
                                            const Placement& placement) = 0;
    virtual core::Result<void> setFixed(document::Document& assembly, const core::Uuid& componentId, bool fixed) = 0;
    // Показывать ли компонент нерассечённым на разрезах чертежа.
    virtual core::Result<void> setWholeInSection(document::Document& assembly, const core::Uuid& componentId,
                                                 bool whole) = 0;

    virtual core::Result<core::Uuid> addMate(document::Document& assembly, const MateParams& params) = 0;
    virtual core::Result<void> removeMate(document::Document& assembly, const core::Uuid& mateId) = 0;

    // Двигает незакреплённые компоненты так, чтобы выполнить сопряжения.
    // Сопряжения применяются по очереди, несколько проходов; противоречивые попадают в отчёт.
    virtual core::Result<SolveReport> solve(document::Document& assembly,
                                            const std::atomic<bool>* cancel = nullptr) = 0;
    // Степени свободы компонентов в нынешнем положении (после solve). Считаются численно:
    // программа пробует малые сдвиги и повороты и смотрит, какие из них не нарушают сопряжений.
    virtual core::Result<std::vector<Freedom>> degreesOfFreedom(const document::Document& assembly) = 0;
    // exploded — тела на местах разнесённого вида (место по сопряжениям плюс сдвиг explode).
    virtual core::Result<std::vector<PlacedBody>> placedBodies(const document::Document& assembly,
                                                                bool exploded = false) = 0;
    // Сдвиг компонента в разнесённом виде. На сопряжения и на собранный вид не влияет.
    virtual core::Result<void> setExplode(document::Document& assembly, const core::Uuid& componentId,
                                          const geometry::Vector3& offset) = 0;
    // Разносит все незакреплённые компоненты сами: каждый отодвигается от закреплённого компонента
    // по направлению между их серединами на distance мм (компонент, стоящий по центру, — вверх).
    // distance = 0 убирает разнос. Возвращает число сдвинутых компонентов.
    virtual core::Result<int> autoExplode(document::Document& assembly, double distance) = 0;
    virtual core::Result<std::vector<Interference>> checkInterference(const document::Document& assembly,
                                                                      const std::atomic<bool>* cancel = nullptr) = 0;
    // Зазор между двумя компонентами, мм; 0 — касаются или пересекаются.
    virtual core::Result<double> clearance(const document::Document& assembly, const core::Uuid& first,
                                           const core::Uuid& second) = 0;
    virtual core::Result<std::vector<BomLine>> billOfMaterials(const document::Document& assembly) = 0;
};

}  // namespace cosmocad::assembly
