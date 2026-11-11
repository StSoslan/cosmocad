// Библиотека стандартного крепежа: болты, гайки, шайбы. Размеры лежат в данных
// (resources/libraries/fasteners.json) со ссылкой на стандарт и источник; в коде размеров нет.
// По записи библиотеки строится обычная параметрическая деталь.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/document/Document.h"
#include "cosmocad/modeling/IModelingService.h"

namespace cosmocad::libraries {

enum class FastenerKind { Bolt, Nut, Washer };

struct Bolt {
    double d = 0, pitch = 0, S = 0, k = 0, e = 0;
    double lengthMin = 0, lengthMax = 0;
    double fullThreadUpTo = 0;               // до этой длины включительно резьба на всём стержне
    double threadTo125 = 0;                  // длина резьбы при длине болта до 125 мм
    std::optional<double> threadTo200;       // при длине 130–200 мм
    bool notRecommended = false;             // диаметр в стандарте дан в скобках
};
struct Nut {
    double d = 0, S = 0, m = 0, e = 0;
    bool notRecommended = false;
};
struct Washer {
    double d = 0, d1 = 0, d2 = 0, s = 0;
};

// Что выбрал пользователь: вид, диаметр резьбы и (для болта) длина.
struct FastenerRequest {
    FastenerKind kind = FastenerKind::Bolt;
    double d = 0;
    double length = 0;
};

// Сведения о записи библиотеки для показа и для состава изделия.
struct FastenerInfo {
    std::string designation;  // «Болт М12×60 ГОСТ 7798-70»
    std::string standard;     // «ГОСТ 7798-70»
    double threadLength = 0;  // болт: длина резьбы; 0 — не применимо
    bool notRecommended = false;
};

class FastenerLibrary {
public:
    static core::Result<FastenerLibrary> load(const std::filesystem::path& file);

    const std::string& version() const { return version_; }
    const std::string& source() const { return source_; }
    const std::vector<Bolt>& bolts() const { return bolts_; }
    const std::vector<Nut>& nuts() const { return nuts_; }
    const std::vector<Washer>& washers() const { return washers_; }
    // Длины болта этого диаметра, которые есть в стандарте.
    std::vector<double> boltLengths(double d) const;
    // Диаметры, для которых есть изделие этого вида.
    std::vector<double> diameters(FastenerKind kind) const;

    // Проверяет, что такое изделие есть в стандарте, и даёт его обозначение.
    core::Result<FastenerInfo> describe(const FastenerRequest& request) const;
    // Строит деталь в пустом документе «Деталь»: тело, обозначение, материал. Резьба не моделируется.
    core::Result<FastenerInfo> build(modeling::IModelingService& modeling, document::Document& part,
                                     const FastenerRequest& request) const;

private:
    std::string version_, source_;
    std::string boltStandard_, nutStandard_, washerStandard_;
    std::vector<Bolt> bolts_;
    std::vector<Nut> nuts_;
    std::vector<Washer> washers_;
    std::vector<double> lengths_, lengthsNotRecommended_;
};

}  // namespace cosmocad::libraries
