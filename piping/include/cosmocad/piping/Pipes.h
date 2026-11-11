// Напорные трубы из полипропилена: напряжение в стенке от внутреннего давления, допустимое
// давление для класса эксплуатации, срок службы. Все числа — в resources/libraries/pipes_pp.json.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/modeling/ModelingService.h"

namespace cosmocad::piping {

// Одна ветвь кривой длительной прочности: lg t = A + B·lg σ / T + C / T + D·lg σ.
struct Branch {
    double a = 0, b = 0, c = 0, d = 0;
};

// Вид участка режима: от него зависит коэффициент запаса.
enum class StageKind { Operating, Maximum, Malfunction, Cold };

struct PipeMaterial {
    std::string name;   // «PP-R»
    std::string title;  // полное название
    double density = 0; // г/см³, справочная
    std::vector<Branch> branches;
    double safetyOperating = 0, safetyMaximum = 0, safetyMalfunction = 0, safetyCold = 0;
    std::map<std::string, double> designStress;  // расчётные напряжения из таблицы стандарта, по классам, МПа
    std::vector<double> seriesPressures;                      // давления, для которых в стандарте есть таблица серий
    std::map<std::string, std::vector<double>> seriesTable;   // класс → наибольшие серии для этих давлений
    double safety(StageKind kind) const;
};

// Участок режима: температура и сколько часов за весь срок труба при ней работает.
struct Stage {
    StageKind kind = StageKind::Operating;
    double temperature = 0;  // °C
    double hours = 0;
};

struct ServiceClass {
    std::string name;   // «1», «2», «4», «5», «ХВ»
    std::string title;  // область применения
    std::vector<Stage> stages;
};

// Итог проверки трубы под давлением.
struct PipeCheck {
    std::string material, serviceClass;
    double diameter = 0, wall = 0;  // мм
    double sdr = 0;                 // отношение диаметра к толщине стенки
    double series = 0;              // S = (d − e) / (2e)
    double nominalSeries = 0;       // серия по размерному ряду (2; 2,5; 3,2; 4; 5); 0 — труба не из ряда
    double maxSeries = 0;           // наибольшая допустимая серия для класса и давления, до 0,1
    bool coldOk = false;            // держит холодную воду 1,0 МПа 50 лет (условие стандарта для классов 1, 2, 4, 5)
    bool tableStress = false;       // допустимое напряжение взято из таблицы стандарта, а не вычислено
    double pressure = 0;            // МПа, заданное
    double stress = 0;              // МПа, напряжение в стенке от заданного давления
    double designStress = 0;        // МПа, допустимое напряжение для класса на расчётный срок
    double allowedPressure = 0;     // МПа, допустимое давление
    double minimumWall = 0;         // мм, наименьшая толщина стенки для заданного давления
    double lifeYears = 0;           // ожидаемый срок службы при заданном давлении; не больше kLifeLimit
    bool ok = false;                // давление не больше допустимого
    std::vector<std::string> lines; // отчёт по строкам, по-русски
};

inline constexpr double kLifeLimitYears = 100.0;  // дальше расчёт не имеет смысла: «более 100 лет»

class PipeLibrary {
public:
    static core::Result<PipeLibrary> load(const std::filesystem::path& file);

    const std::string& standard() const { return standard_; }
    const std::string& source() const { return source_; }
    double designLifeYears() const { return designLifeYears_; }
    const std::vector<PipeMaterial>& materials() const { return materials_; }
    const std::vector<ServiceClass>& classes() const { return classes_; }
    const std::vector<double>& diameters() const { return diameters_; }
    std::vector<double> sdrs() const;

    const PipeMaterial* material(const std::string& name) const;
    const ServiceClass* serviceClass(const std::string& name) const;

    // Толщина стенки трубы размерного ряда, мм.
    core::Result<double> wall(double diameter, double sdr) const;
    // Напряжение в стенке, МПа: σ = p·(d − e) / (2e). Давление в МПа, размеры в мм.
    static core::Result<double> hoopStress(double pressure, double diameter, double wall);
    // Сколько часов труба выдержит постоянное напряжение при постоянной температуре (без запаса).
    core::Result<double> hoursToFailure(const std::string& material, double temperature, double stress) const;
    // Допустимое (расчётное) напряжение для класса эксплуатации, МПа: из таблицы стандарта,
    // а если класса в таблице нет — вычисленное.
    core::Result<double> designStress(const std::string& material, const std::string& serviceClass) const;
    // То же, вычисленное по правилу Майнера из уравнений длительной прочности.
    core::Result<double> computedDesignStress(const std::string& material, const std::string& serviceClass) const;
    // Наибольшая допустимая серия S для класса и рабочего давления (формулы 6 и 7 стандарта), до 0,1.
    core::Result<double> maxSeries(const std::string& material, const std::string& serviceClass, double pressure) const;
    // Срок службы в годах при напряжении stress и режиме класса (доли времени — как в классе).
    core::Result<double> lifeYears(const std::string& material, const std::string& serviceClass, double stress) const;
    // Полная проверка: труба d × e, давление в МПа.
    core::Result<PipeCheck> check(const std::string& material, const std::string& serviceClass, double diameter, double wall,
                                  double pressure) const;

    // Обозначение отрезка трубы: «Труба PP-R 25×4,2 L=1000».
    std::string designation(const std::string& material, double diameter, double wall, double length) const;
    // Строит отрезок трубы в пустом документе «Деталь».
    core::Result<void> build(modeling::IModelingService& modeling, document::Document& part, const std::string& material,
                             double diameter, double wall, double length) const;

private:
    struct Series {
        double sdr = 0, s = 0;
    };
    double damagePerHour(const PipeMaterial& material, const ServiceClass& serviceClass, double stress) const;
    std::string standard_, source_;
    double designLifeYears_ = 50;
    double minTemperature_ = 10, maxTemperature_ = 110;
    std::vector<PipeMaterial> materials_;
    std::vector<ServiceClass> classes_;
    std::vector<double> diameters_;
    std::vector<Series> series_;
    std::map<double, std::vector<double>> walls_;  // диаметр → толщины по порядку серий (0 — такой трубы нет)
    double coldPressure_ = 1.0;                    // МПа, давление проверки холодной водой
};

}  // namespace cosmocad::piping
