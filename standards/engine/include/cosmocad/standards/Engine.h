// Standards Engine — проверка оформления документа по профилю стандарта (нормоконтроль).
// Не зависит ни от интерфейса пользователя, ни от 3D-геометрического ядра.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cosmocad/core/Uuid.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/drawing/Entities.h"
#include "cosmocad/geometry/Geometry2D.h"
#include "cosmocad/standards/Profile.h"

namespace cosmocad::standards {

inline constexpr int kStandardsServiceApiVersion = 1;

struct Violation {
    std::string ruleId;
    Severity severity = Severity::Warning;
    core::Uuid objectId;    // пустой номер — замечание к документу в целом
    std::string message;
    std::string reference;
};

// Размеры листа, рамки и основной надписи для данного листа и профиля.
struct SheetGeometry {
    double width = 0.0;
    double height = 0.0;
    geometry::BBox2 frame;       // внутренняя рамка
    geometry::BBox2 titleBlock;  // основная надпись (правый нижний угол рамки)
};

std::optional<SheetGeometry> sheetGeometry(const drawing::Sheet& sheet, const Profile& profile);

// Итоговый вид линии для элемента с учётом слоя, типа линии и толщины основной линии листа.
struct ResolvedLineStyle {
    std::string lineTypeId;
    double width = 0.5;           // мм
    std::vector<double> pattern;  // мм; пусто — сплошная
    std::string color = "#000000";
    bool visible = true;
    bool knownLineType = true;
};

// Толщина основной линии s по листу документа (0,8 мм, если листа нет).
double mainLineWidthOf(const document::Document& document);

// mainLineWidth — значение mainLineWidthOf(document). Его передают явно, чтобы при обходе
// всех элементов не искать лист в документе заново для каждого элемента.
ResolvedLineStyle resolveLineStyle(const document::Document& document, const drawing::Entity& entity,
                                   const Profile& profile, double mainLineWidth);

inline ResolvedLineStyle resolveLineStyle(const document::Document& document, const drawing::Entity& entity,
                                          const Profile& profile) {
    return resolveLineStyle(document, entity, profile, mainLineWidthOf(document));
}

class IStandardsService {
public:
    virtual ~IStandardsService() = default;

    virtual std::vector<std::string> profileIds() const = 0;
    virtual Result<Profile> profile(const std::string& id) const = 0;

    // Проверка документа. Документ не изменяется. Результат зависит только
    // от документа и профиля — повторный вызов даёт тот же список.
    virtual Result<std::vector<Violation>> check(const document::Document& document) const = 0;
};

class StandardsService final : public IStandardsService {
public:
    explicit StandardsService(ProfileRegistry registry) : registry_(std::move(registry)) {}

    std::vector<std::string> profileIds() const override { return registry_.ids(); }
    Result<Profile> profile(const std::string& id) const override { return registry_.resolve(id); }
    Result<std::vector<Violation>> check(const document::Document& document) const override;

private:
    ProfileRegistry registry_;
};

// Проверка документа по уже загруженному профилю.
std::vector<Violation> checkDocument(const document::Document& document, const Profile& profile);

}  // namespace cosmocad::standards
