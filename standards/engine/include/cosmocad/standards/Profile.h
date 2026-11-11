// Профиль стандарта: настройки оформления и правила проверки, загружаемые из файла.
// Профиль — данные, а не код: его можно сменить или дополнить без пересборки программы.
// Тексты стандартов в профиль не входят — только параметры, номера правил и ссылки на пункты.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cosmocad/core/Result.h"

namespace cosmocad::standards {

using core::Result;

struct SheetFormat {
    std::string name;
    double width = 0.0;   // мм, книжная ориентация
    double height = 0.0;  // мм
};

struct FrameMargins {
    double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
};

// Основная надпись задаётся в профиле целиком: линии сетки, постоянные подписи и места
// для заполняемых полей. Координаты — в миллиметрах от левого нижнего угла надписи.
struct TitleBlockLine {
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    bool thin = false;  // тонкая линия; иначе основная
};
struct TitleBlockLabel {
    double x = 0.0, y = 0.0;
    std::string text;
    double height = 2.5;
};
struct TitleBlockField {
    std::string key;    // имя поля в листе, например "designation"
    std::string title;  // название поля для окна настройки листа
    double x = 0.0, y = 0.0;
    double height = 3.5;
};

struct TitleBlockSpec {
    double width = 0.0;
    double height = 0.0;
    std::vector<std::string> requiredFields;
    std::vector<TitleBlockLine> lines;
    std::vector<TitleBlockLabel> labels;
    std::vector<TitleBlockField> fields;
};

enum class LineClass { Main, Thin };

struct LineType {
    std::string id;
    std::string name;
    LineClass lineClass = LineClass::Thin;
    std::vector<double> pattern;  // штрих/пробел в долях толщины; пусто — сплошная
};

enum class Severity { Info, Warning, Error };
const char* toString(Severity severity);

struct RuleSpec {
    std::string id;         // номер правила, например ESKD-LINE-001
    std::string type;       // вид проверки (см. Engine.cpp)
    Severity severity = Severity::Warning;
    std::string message;    // объяснение нарушения
    std::string reference;  // ссылка на пункт стандарта (без текста стандарта)
    nlohmann::json params;  // параметры проверки
};

struct Profile {
    std::string id;
    std::string name;
    std::string version;
    std::string extends;  // id базового профиля; пусто — самостоятельный
    std::vector<SheetFormat> sheetFormats;
    FrameMargins frame;
    TitleBlockSpec titleBlock;
    std::vector<LineType> lineTypes;
    std::vector<double> textHeights;
    std::vector<RuleSpec> rules;

    const SheetFormat* findFormat(const std::string& name) const;
    const LineType* findLineType(const std::string& id) const;
};

// Реестр профилей. Читает каталоги вида <каталог>/<id>/profile.json.
class ProfileRegistry {
public:
    // Загружает все профили из каталога. Профиль с тем же id заменяет прежний
    // (так пользовательский каталог перекрывает поставляемый).
    Result<void> loadDirectory(const std::filesystem::path& directory);
    Result<void> loadFile(const std::filesystem::path& file);
    Result<void> loadJson(const nlohmann::json& json);

    std::vector<std::string> ids() const;

    // Возвращает профиль с применённым наследованием (extends).
    Result<Profile> resolve(const std::string& id) const;

private:
    std::map<std::string, nlohmann::json> raw_;
};

}  // namespace cosmocad::standards
