#include "cosmocad/piping/Pipes.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cosmocad::piping {

using core::ErrorCode;
using core::makeError;
using core::Result;
using nlohmann::json;

namespace {

const double kHoursInYear = 8760.0;

std::string plain(double v, int digits = 9) {
    if (std::abs(v) < 1e-9) return "0";
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(digits);
    out << v;
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text;
}

// Число для человека: запятая вместо точки.
std::string shown(double v, int digits = 2) {
    std::string text = plain(v, digits);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

// «1 год», «2,5 года», «49 лет».
std::string years(double value) {
    const std::string number = shown(value, 1);
    if (std::abs(value - std::round(value)) > 1e-9) return number + " года";
    const long whole = std::lround(value);
    if (whole % 10 == 1 && whole % 100 != 11) return number + " год";
    if (whole % 10 >= 2 && whole % 10 <= 4 && (whole % 100 < 12 || whole % 100 > 14)) return number + " года";
    return number + " лет";
}

bool good(double v) { return std::isfinite(v) && v > 0.0; }

// lg t для одной температуры и напряжения; при двух ветвях — меньшее.
double logHours(const PipeMaterial& material, double temperature, double stress) {
    const double kelvin = temperature + 273.15;
    const double ls = std::log10(stress);
    double best = 1e300;
    for (const auto& branch : material.branches)
        best = std::min(best, branch.a + branch.b * ls / kelvin + branch.c / kelvin + branch.d * ls);
    return best;
}

modeling::SketchShape circle(double diameter, bool hole) {
    modeling::SketchShape shape;
    shape.kind = modeling::SketchShape::Kind::Circle;
    shape.hole = hole;
    shape.values = {"0", "0", plain(diameter / 2.0)};
    return shape;
}

}  // namespace

double PipeMaterial::safety(StageKind kind) const {
    switch (kind) {
        case StageKind::Operating: return safetyOperating;
        case StageKind::Maximum: return safetyMaximum;
        case StageKind::Malfunction: return safetyMalfunction;
        case StageKind::Cold: return safetyCold;
    }
    return safetyOperating;
}

Result<PipeLibrary> PipeLibrary::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть данные о трубах " + file.string());
    PipeLibrary library;
    try {
        const json j = json::parse(in);
        library.standard_ = j.at("standard").get<std::string>();
        library.source_ = j.value("source", std::string());
        library.designLifeYears_ = j.at("designLifeYears").get<double>();
        if (j.contains("temperatureRange")) {
            library.minTemperature_ = j.at("temperatureRange").at(0).get<double>();
            library.maxTemperature_ = j.at("temperatureRange").at(1).get<double>();
        }
        if (!good(library.designLifeYears_)) return makeError(ErrorCode::ParseError, "данные о трубах: расчётный срок должен быть больше нуля");
        for (const auto& item : j.at("materials")) {
            PipeMaterial m;
            m.name = item.at("name").get<std::string>();
            m.title = item.value("title", m.name);
            m.density = item.at("density").get<double>();
            for (const auto& b : item.at("branches"))
                m.branches.push_back({b.at("A").get<double>(), b.at("B").get<double>(), b.at("C").get<double>(), b.at("D").get<double>()});
            const auto& safety = item.at("safety");
            m.safetyOperating = safety.at("operating").get<double>();
            m.safetyMaximum = safety.at("maximum").get<double>();
            m.safetyMalfunction = safety.at("malfunction").get<double>();
            m.safetyCold = safety.at("cold").get<double>();
            if (item.contains("designStress"))
                for (const auto& [className, value] : item.at("designStress").items()) m.designStress[className] = value.get<double>();
            if (item.contains("maxSeries")) {
                const auto& table = item.at("maxSeries");
                m.seriesPressures = table.at("pressures").get<std::vector<double>>();
                for (const auto& [className, values] : table.items()) {
                    if (!values.is_array() || className == "pressures") continue;
                    m.seriesTable[className] = values.get<std::vector<double>>();
                    if (m.seriesTable[className].size() != m.seriesPressures.size())
                        return makeError(ErrorCode::ParseError, "данные о трубах: таблица серий материала «" + m.name + "» неполная");
                }
            }
            if (m.name.empty() || m.branches.empty() || !good(m.density) || m.safetyOperating < 1.0 || m.safetyMaximum < 1.0 ||
                m.safetyMalfunction < 1.0 || m.safetyCold < 1.0)
                return makeError(ErrorCode::ParseError, "данные о трубах: неполная запись материала «" + m.name + "» (запас прочности не меньше 1)");
            library.materials_.push_back(std::move(m));
        }
        for (const auto& item : j.at("classes")) {
            ServiceClass c;
            c.name = item.at("name").get<std::string>();
            c.title = item.value("title", std::string());
            for (const auto& s : item.at("stages")) {
                Stage stage;
                const std::string kind = s.at("kind").get<std::string>();
                if (kind == "operating")
                    stage.kind = StageKind::Operating;
                else if (kind == "maximum")
                    stage.kind = StageKind::Maximum;
                else if (kind == "malfunction")
                    stage.kind = StageKind::Malfunction;
                else if (kind == "cold")
                    stage.kind = StageKind::Cold;
                else
                    return makeError(ErrorCode::ParseError, "данные о трубах: неизвестный вид участка «" + kind + "»");
                stage.temperature = s.at("t").get<double>();
                stage.hours = s.contains("hours") ? s.at("hours").get<double>() : s.at("years").get<double>() * kHoursInYear;
                if (!good(stage.hours) || stage.temperature < library.minTemperature_ || stage.temperature > library.maxTemperature_)
                    return makeError(ErrorCode::ParseError, "данные о трубах: неверный участок класса «" + c.name + "»");
                c.stages.push_back(stage);
            }
            if (c.name.empty() || c.stages.empty()) return makeError(ErrorCode::ParseError, "данные о трубах: пустой класс эксплуатации");
            library.classes_.push_back(std::move(c));
        }
        library.coldPressure_ = j.value("coldPressureCheck", 1.0);
        const auto& sizes = j.at("sizes");
        for (const auto& item : sizes.at("series")) {
            Series series{item.at("sdr").get<double>(), item.at("S").get<double>()};
            if (!good(series.sdr) || !good(series.s)) return makeError(ErrorCode::ParseError, "данные о трубах: неверная серия");
            library.series_.push_back(series);
        }
        for (const auto& [diameter, values] : sizes.at("walls").items()) {
            const double d = std::stod(diameter);
            std::vector<double> walls;
            for (const auto& value : values) walls.push_back(value.is_null() ? 0.0 : value.get<double>());
            if (!good(d) || walls.size() != library.series_.size())
                return makeError(ErrorCode::ParseError, "данные о трубах: у диаметра " + diameter + " число толщин не равно числу серий");
            library.walls_[d] = std::move(walls);
            library.diameters_.push_back(d);
        }
        std::sort(library.diameters_.begin(), library.diameters_.end());
    } catch (const json::exception& problem) {
        return makeError(ErrorCode::ParseError, "данные о трубах " + file.string() + ": " + problem.what());
    }
    if (library.materials_.empty() || library.classes_.empty())
        return makeError(ErrorCode::ParseError, "данные о трубах: нет материалов или классов эксплуатации");
    return library;
}

std::vector<double> PipeLibrary::sdrs() const {
    std::vector<double> out;
    for (const auto& series : series_) out.push_back(series.sdr);
    return out;
}

const PipeMaterial* PipeLibrary::material(const std::string& name) const {
    for (const auto& m : materials_)
        if (m.name == name) return &m;
    return nullptr;
}

const ServiceClass* PipeLibrary::serviceClass(const std::string& name) const {
    for (const auto& c : classes_)
        if (c.name == name) return &c;
    // «ХВ» удобно набирать и латиницей.
    if (name == "XB" || name == "xb" || name == "хв") return serviceClass("ХВ");
    return nullptr;
}

Result<double> PipeLibrary::wall(double diameter, double sdr) const {
    const auto row = std::find_if(walls_.begin(), walls_.end(), [&](const auto& entry) { return std::abs(entry.first - diameter) < 1e-9; });
    if (row == walls_.end()) return makeError(ErrorCode::NotFound, "в размерном ряду нет трубы диаметром " + shown(diameter) + " мм");
    for (std::size_t i = 0; i < series_.size(); ++i)
        if (std::abs(series_[i].sdr - sdr) < 1e-9) {
            if (!good(row->second[i]))
                return makeError(ErrorCode::NotFound, "в стандарте нет трубы " + shown(diameter) + " мм серии SDR " + shown(sdr));
            return row->second[i];
        }
    return makeError(ErrorCode::NotFound, "в размерном ряду нет серии SDR " + shown(sdr));
}

Result<double> PipeLibrary::hoopStress(double pressure, double diameter, double wall) {
    if (!good(diameter) || !good(wall)) return makeError(ErrorCode::InvalidArgument, "диаметр и толщина стенки должны быть больше нуля");
    if (wall * 2.0 >= diameter) return makeError(ErrorCode::InvalidArgument, "стенка слишком толстая: две толщины не меньше диаметра");
    if (!std::isfinite(pressure) || pressure < 0.0) return makeError(ErrorCode::InvalidArgument, "давление не может быть отрицательным");
    return pressure * (diameter - wall) / (2.0 * wall);
}

Result<double> PipeLibrary::hoursToFailure(const std::string& materialName, double temperature, double stress) const {
    const PipeMaterial* m = material(materialName);
    if (!m) return makeError(ErrorCode::NotFound, "нет данных о материале «" + materialName + "»");
    if (!std::isfinite(temperature) || temperature < minTemperature_ || temperature > maxTemperature_)
        return makeError(ErrorCode::InvalidArgument, "температура должна быть от " + shown(minTemperature_) + " до " + shown(maxTemperature_) +
                                                         " °C: вне этих пределов кривые прочности не действуют");
    if (!good(stress)) return makeError(ErrorCode::InvalidArgument, "напряжение должно быть больше нуля");
    return std::pow(10.0, std::min(logHours(*m, temperature, stress), 300.0));
}

// Доля ресурса, расходуемая за час работы по режиму класса (правило Майнера).
double PipeLibrary::damagePerHour(const PipeMaterial& m, const ServiceClass& c, double stress) const {
    double total = 0.0, damage = 0.0;
    for (const auto& stage : c.stages) total += stage.hours;
    for (const auto& stage : c.stages) {
        const double lg = logHours(m, stage.temperature, stress * m.safety(stage.kind));
        damage += (stage.hours / total) / std::pow(10.0, std::clamp(lg, -300.0, 300.0));
    }
    return damage;
}

Result<double> PipeLibrary::lifeYears(const std::string& materialName, const std::string& className, double stress) const {
    const PipeMaterial* m = material(materialName);
    if (!m) return makeError(ErrorCode::NotFound, "нет данных о материале «" + materialName + "»");
    const ServiceClass* c = serviceClass(className);
    if (!c) return makeError(ErrorCode::NotFound, "нет класса эксплуатации «" + className + "»");
    if (!good(stress)) return kLifeLimitYears;
    const double damage = damagePerHour(*m, *c, stress);
    if (!(damage > 0.0)) return kLifeLimitYears;
    return std::min(kLifeLimitYears, 1.0 / damage / kHoursInYear);
}

Result<double> PipeLibrary::designStress(const std::string& materialName, const std::string& className) const {
    const PipeMaterial* m = material(materialName);
    const ServiceClass* c = serviceClass(className);
    if (m && c)
        if (const auto it = m->designStress.find(c->name); it != m->designStress.end()) return it->second;
    return computedDesignStress(materialName, className);
}

Result<double> PipeLibrary::maxSeries(const std::string& materialName, const std::string& className, double pressure) const {
    if (!good(pressure)) return makeError(ErrorCode::InvalidArgument, "давление должно быть больше нуля");
    auto design = designStress(materialName, className);
    if (!design) return design.error();
    const ServiceClass* c = serviceClass(className);
    // Для давлений из ряда стандарта значение берётся прямо из его таблицы.
    const PipeMaterial* m = material(materialName);
    if (const auto row = m->seriesTable.find(c->name); row != m->seriesTable.end())
        for (std::size_t i = 0; i < m->seriesPressures.size(); ++i)
            if (std::abs(m->seriesPressures[i] - pressure) < 1e-9) return row->second[i];
    double limit = design.value() / pressure;
    // Классы горячей воды и отопления: труба должна держать и холодную воду под давлением 1,0 МПа.
    if (c->stages.front().kind != StageKind::Cold) {
        const std::string coldName = "ХВ";
        if (serviceClass(coldName)) limit = std::min(limit, designStress(materialName, coldName).value() / coldPressure_);
    }
    return std::floor(limit * 10.0 + 0.5 + 1e-9) / 10.0;
}

Result<double> PipeLibrary::computedDesignStress(const std::string& materialName, const std::string& className) const {
    const PipeMaterial* m = material(materialName);
    if (!m) return makeError(ErrorCode::NotFound, "нет данных о материале «" + materialName + "»");
    const ServiceClass* c = serviceClass(className);
    if (!c) return makeError(ErrorCode::NotFound, "нет класса эксплуатации «" + className + "»");
    // Напряжение, при котором ресурс расходуется ровно за расчётный срок. Чем больше напряжение,
    // тем быстрее расход, поэтому ищем делением отрезка пополам.
    const double target = 1.0 / (designLifeYears_ * kHoursInYear);
    double low = 0.01, high = 100.0;
    for (int i = 0; i < 200; ++i) {
        const double middle = 0.5 * (low + high);
        (damagePerHour(*m, *c, middle) > target ? high : low) = middle;
    }
    return low;
}

Result<PipeCheck> PipeLibrary::check(const std::string& materialName, const std::string& className, double diameter, double wallThickness,
                                     double pressure) const {
    const PipeMaterial* m = material(materialName);
    if (!m) return makeError(ErrorCode::NotFound, "нет данных о материале «" + materialName + "»");
    const ServiceClass* c = serviceClass(className);
    if (!c) return makeError(ErrorCode::NotFound, "нет класса эксплуатации «" + className + "»");
    auto stress = hoopStress(pressure, diameter, wallThickness);
    if (!stress) return stress.error();
    auto design = designStress(m->name, c->name);
    if (!design) return design.error();

    PipeCheck r;
    r.material = m->name;
    r.serviceClass = c->name;
    r.diameter = diameter;
    r.wall = wallThickness;
    r.sdr = diameter / wallThickness;
    r.series = (diameter - wallThickness) / (2.0 * wallThickness);
    // Труба из размерного ряда сравнивается по своей номинальной серии, как в стандарте.
    if (const auto row = std::find_if(walls_.begin(), walls_.end(), [&](const auto& entry) { return std::abs(entry.first - diameter) < 1e-9; });
        row != walls_.end())
        for (std::size_t i = 0; i < series_.size(); ++i)
            if (good(row->second[i]) && std::abs(row->second[i] - wallThickness) < 1e-9 && r.nominalSeries == 0.0) r.nominalSeries = series_[i].s;
    const double usedSeries = r.nominalSeries > 0.0 ? r.nominalSeries : r.series;
    r.pressure = pressure;
    r.stress = stress.value();
    r.designStress = design.value();
    r.tableStress = m->designStress.count(c->name) > 0;
    r.allowedPressure = design.value() / usedSeries;
    r.minimumWall = pressure * diameter / (2.0 * design.value() + pressure);
    r.lifeYears = lifeYears(m->name, c->name, r.stress).value();

    const bool hot = c->stages.front().kind != StageKind::Cold;
    double coldLimit = 0.0;  // наибольшая серия по условию холодной воды
    r.coldOk = true;
    if (hot && serviceClass("ХВ")) {
        coldLimit = std::floor(designStress(m->name, "ХВ").value() / coldPressure_ * 10.0 + 0.5 + 1e-9) / 10.0;
        r.coldOk = usedSeries <= coldLimit + 1e-9;
    }
    bool pressureOk = true;
    if (pressure > 0.0) {
        r.maxSeries = maxSeries(m->name, c->name, pressure).value();
        // Для трубы из ряда — сравнение серий с округлением до 0,1, как в стандарте; иначе — по давлению.
        pressureOk = r.nominalSeries > 0.0 ? usedSeries <= r.maxSeries + 1e-9 : pressure <= r.allowedPressure + 1e-12;
    }
    r.ok = pressureOk && r.coldOk;

    const auto bar = [](double mpa) { return shown(mpa, 2) + " МПа (" + shown(mpa * 10.0, 1) + " бар)"; };
    r.lines.push_back("Труба " + m->name + " " + shown(diameter) + "×" + shown(wallThickness) + " мм, SDR " + shown(r.sdr, 1) + ", серия S " +
                      (r.nominalSeries > 0.0 ? shown(r.nominalSeries, 1) + " (по размерному ряду)" : shown(r.series, 2)));
    r.lines.push_back("Класс эксплуатации " + c->name + (c->title.empty() ? "" : " — " + c->title));
    for (const auto& stage : c->stages) {
        const char* what = stage.kind == StageKind::Operating ? "рабочая" : stage.kind == StageKind::Maximum ? "максимальная"
                           : stage.kind == StageKind::Malfunction ? "аварийная" : "холодная вода";
        const std::string time = stage.hours >= kHoursInYear ? years(stage.hours / kHoursInYear) : shown(stage.hours, 0) + " ч";
        r.lines.push_back("  " + shown(stage.temperature, 0) + " °C, " + what + " — " + time + ", запас " + shown(m->safety(stage.kind), 2));
    }
    r.lines.push_back("Давление: " + bar(pressure));
    r.lines.push_back("Напряжение в стенке: " + shown(r.stress, 2) + " МПа");
    r.lines.push_back("Допустимое напряжение на " + years(designLifeYears_) + ": " + shown(r.designStress, 2) + " МПа" +
                      (r.tableStress ? " (таблица стандарта)" : " (вычислено)"));
    if (pressure > 0.0)
        r.lines.push_back("Наибольшая допустимая серия S для этого давления: " + shown(r.maxSeries, 1));
    r.lines.push_back("Допустимое давление: " + bar(r.allowedPressure));
    if (hot && coldLimit > 0.0)
        r.lines.push_back(std::string("Холодная вода ") + shown(coldPressure_, 1) + " МПа, 50 лет (обязательное условие): " +
                          (r.coldOk ? "держит" : "НЕ держит — серия должна быть не больше " + shown(coldLimit, 1)));
    r.lines.push_back("Наименьшая толщина стенки для этого давления: " + shown(r.minimumWall, 2) + " мм");
    if (r.lifeYears >= kLifeLimitYears)
        r.lines.push_back("Срок службы по формулам длительной прочности: более " + years(kLifeLimitYears));
    else if (r.lifeYears * kHoursInYear < 1.0)
        r.lines.push_back("Срок службы по формулам длительной прочности: меньше часа");
    else if (r.lifeYears < 0.1)
        r.lines.push_back("Срок службы по формулам длительной прочности: " + shown(r.lifeYears * kHoursInYear, 0) + " ч");
    else
        r.lines.push_back("Срок службы по формулам длительной прочности: " +
                          years(r.lifeYears < 10 ? std::round(r.lifeYears * 10.0) / 10.0 : std::round(r.lifeYears)));
    if (r.ok && r.lifeYears < designLifeYears_ - 0.5)
        r.lines.push_back("  По таблице стандарта труба подходит, хотя формулы дают срок меньше " + years(designLifeYears_) +
                          ": табличное напряжение для этого класса чуть выше вычисленного.");
    r.lines.push_back(r.ok ? "Итог: труба подходит." : !r.coldOk && pressureOk ? "Итог: труба НЕ подходит — не держит холодную воду 1,0 МПа."
                                                                             : "Итог: труба НЕ подходит — давление больше допустимого.");
    r.lines.push_back("Расчёт по " + standard_ + "; не учитывает гидроудар, изгиб, соединения и качество монтажа.");
    r.lines.push_back("Толщину стенки сверьте с маркировкой трубы: у армированных труб расчёт ведёт производитель.");
    return r;
}

std::string PipeLibrary::designation(const std::string& materialName, double diameter, double wallThickness, double length) const {
    return "Труба " + materialName + " " + shown(diameter) + "×" + shown(wallThickness) + " L=" + shown(length, 1);
}

Result<void> PipeLibrary::build(modeling::IModelingService& modeling, document::Document& part, const std::string& materialName,
                                double diameter, double wallThickness, double length) const {
    const PipeMaterial* m = material(materialName);
    if (!m) return makeError(ErrorCode::NotFound, "нет данных о материале «" + materialName + "»");
    if (auto valid = hoopStress(0.0, diameter, wallThickness); !valid) return valid.error();
    if (!good(length)) return makeError(ErrorCode::InvalidArgument, "длина трубы должна быть больше нуля");
    if (!modeling.features(part).empty()) return makeError(ErrorCode::InvalidState, "деталь уже содержит построения");
    auto sketch = modeling.addSketch(part, {"Сечение трубы", "XY", "0", "", {circle(diameter, false), circle(diameter - 2.0 * wallThickness, true)}});
    if (!sketch) return sketch.error();
    if (auto f = modeling.addExtrude(part, {sketch.value(), plain(length), false, false}); !f) return f.error();
    if (auto ok = modeling.setPartProperties(part, designation(materialName, diameter, wallThickness, length), m->title, m->density); !ok)
        return ok.error();
    return {};
}

}  // namespace cosmocad::piping
