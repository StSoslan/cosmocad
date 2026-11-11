// Библиотека проката: уголки равнополочные, швеллеры и двутавры с параллельными гранями полок. Размеры — в resources/libraries/profiles.json.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/modeling/ModelingService.h"

namespace cosmocad::libraries {

// Уголок равнополочный: ширина полки b, толщина t, радиус внутреннего закругления R, радиус закругления полок r.
struct AngleSize {
    double b = 0, t = 0, R = 0, r = 0;
    double area = 0;  // см², из таблицы стандарта
    double mass = 0;  // кг, масса 1 м из таблицы стандарта
};

// Швеллер с параллельными гранями полок: высота h, ширина полки b, толщина стенки s и полки t,
// радиус внутреннего закругления R, радиус закругления полки r.
struct ChannelSize {
    std::string name;    // «16П»
    std::string series;  // «П», «Э», «Л»
    double h = 0, b = 0, s = 0, t = 0, R = 0, r = 0;
    double area = 0, mass = 0;
};

// Двутавр с параллельными гранями полок: высота h, ширина полки b, толщина стенки s и полки t, радиус сопряжения r.
struct BeamSize {
    std::string name;  // «20Б1»
    std::string type;  // «Б», «Ш», «К», «С», «ДБ», «ДК»
    double h = 0, b = 0, s = 0, t = 0, r = 0;
    double area = 0;
    double mass = 0;  // 0 — в таблице не прочиталась
};

class ProfileLibrary {
public:
    static core::Result<ProfileLibrary> load(const std::filesystem::path& file);

    const std::string& angleStandard() const { return angleStandard_; }
    const std::vector<AngleSize>& angles() const { return angles_; }
    std::vector<double> angleWidths() const;
    std::vector<double> angleThicknesses(double b) const;
    const AngleSize* angle(double b, double t) const;

    // «Уголок 50×50×5 ГОСТ 8509-93 L=1000».
    core::Result<std::string> angleDesignation(double b, double t, double length) const;
    // Строит отрезок уголка в пустом документе «Деталь». Закругления заменены короткими отрезками.
    core::Result<void> buildAngle(modeling::IModelingService& modeling, document::Document& part, double b, double t, double length) const;

    const std::string& channelStandard() const { return channelStandard_; }
    const std::vector<ChannelSize>& channels() const { return channels_; }
    const ChannelSize* channel(const std::string& name) const;
    const std::string& beamStandard() const { return beamStandard_; }
    const std::vector<BeamSize>& beams() const { return beams_; }
    const BeamSize* beam(const std::string& name) const;
    // Типы двутавров по порядку таблицы: обозначение и название («Б» — «балочные нормальные»).
    const std::vector<std::pair<std::string, std::string>>& beamTypes() const { return beamTypes_; }

    // Швеллер или двутавр: kind — "channel" или "beam", name — номер профиля.
    // «Швеллер 16П ГОСТ 8240-97 L=1000», «Двутавр 20Б1 ГОСТ Р 57837-2017 L=1000».
    core::Result<std::string> designation(const std::string& kind, const std::string& name, double length) const;
    core::Result<void> build(modeling::IModelingService& modeling, document::Document& part, const std::string& kind,
                             const std::string& name, double length) const;

private:
    std::string channelStandard_, beamStandard_;
    std::vector<ChannelSize> channels_;
    std::vector<BeamSize> beams_;
    std::vector<std::pair<std::string, std::string>> beamTypes_;
    std::string angleStandard_;
    std::vector<AngleSize> angles_;
};

}  // namespace cosmocad::libraries
