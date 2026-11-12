#include "cosmocad/standards/Profile.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <fstream>
#include <set>

namespace cosmocad::standards {

using core::ErrorCode;
using core::makeError;
using nlohmann::json;

const char* toString(Severity severity) {
    switch (severity) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "warning";
}

const SheetFormat* Profile::findFormat(const std::string& formatName) const {
    for (const auto& format : sheetFormats)
        if (format.name == formatName) return &format;
    return nullptr;
}

const LineType* Profile::findLineType(const std::string& lineTypeId) const {
    for (const auto& type : lineTypes)
        if (type.id == lineTypeId) return &type;
    return nullptr;
}

namespace {

Severity severityFromString(const std::string& text) {
    if (text == "error") return Severity::Error;
    if (text == "info") return Severity::Info;
    return Severity::Warning;
}

RuleSpec ruleFromJson(const json& j) {
    RuleSpec rule;
    rule.id = j.at("id").get<std::string>();
    rule.type = j.at("type").get<std::string>();
    rule.severity = severityFromString(j.value("severity", std::string("warning")));
    rule.message = j.value("message", std::string());
    rule.reference = j.value("reference", std::string());
    rule.params = j.value("params", json::object());
    return rule;
}

Profile profileFromJson(const json& j) {
    Profile p;
    p.id = j.at("id").get<std::string>();
    p.name = j.value("name", p.id);
    p.version = j.value("version", std::string("0"));
    p.extends = j.value("extends", std::string());
    for (const auto& f : j.value("sheetFormats", json::array()))
        p.sheetFormats.push_back({f.at("name").get<std::string>(), f.at("width").get<double>(),
                                  f.at("height").get<double>()});
    if (j.contains("frame")) {
        const auto& f = j.at("frame");
        p.frame = {f.value("left", 0.0), f.value("top", 0.0), f.value("right", 0.0), f.value("bottom", 0.0)};
    }
    if (j.contains("titleBlock")) {
        const auto& t = j.at("titleBlock");
        p.titleBlock.width = t.value("width", 0.0);
        p.titleBlock.height = t.value("height", 0.0);
        p.titleBlock.requiredFields = t.value("requiredFields", std::vector<std::string>{});
        for (const auto& l : t.value("lines", json::array()))
            p.titleBlock.lines.push_back({l.at(0).get<double>(), l.at(1).get<double>(), l.at(2).get<double>(),
                                          l.at(3).get<double>(), l.size() > 4 && l.at(4).get<std::string>() == "thin"});
        for (const auto& l : t.value("labels", json::array()))
            p.titleBlock.labels.push_back({l.at("x").get<double>(), l.at("y").get<double>(),
                                           l.at("text").get<std::string>(), l.value("height", 2.5)});
        for (const auto& f : t.value("fields", json::array()))
            p.titleBlock.fields.push_back({f.at("key").get<std::string>(), f.value("title", f.at("key").get<std::string>()),
                                           f.at("x").get<double>(), f.at("y").get<double>(), f.value("height", 3.5)});
    }
    for (const auto& t : j.value("lineTypes", json::array())) {
        LineType type;
        type.id = t.at("id").get<std::string>();
        type.name = t.value("name", type.id);
        type.lineClass = t.value("class", std::string("thin")) == "main" ? LineClass::Main : LineClass::Thin;
        type.pattern = t.value("pattern", std::vector<double>{});  // штрих/пробел, мм
        p.lineTypes.push_back(std::move(type));
    }
    p.textHeights = j.value("textHeights", std::vector<double>{});

    std::set<std::string> disabled;
    for (const auto& id : j.value("rulesDisable", json::array())) disabled.insert(id.get<std::string>());
    for (const auto& r : j.value("rules", json::array())) {
        RuleSpec rule = ruleFromJson(r);
        if (!disabled.count(rule.id)) p.rules.push_back(std::move(rule));
    }
    for (const auto& r : j.value("rulesAppend", json::array())) p.rules.push_back(ruleFromJson(r));
    return p;
}

}  // namespace

Result<void> ProfileRegistry::loadJson(const json& j) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string())
        return makeError(ErrorCode::ParseError, "профиль: не указан id");
    raw_[j["id"].get<std::string>()] = j;
    return {};
}

Result<void> ProfileRegistry::loadFile(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть профиль: " + file.string());
    json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return makeError(ErrorCode::ParseError, "профиль повреждён: " + file.string());
    return loadJson(j);
}

Result<void> ProfileRegistry::loadDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
        return makeError(ErrorCode::NotFound, "каталог профилей не найден: " + directory.string());
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        auto candidate = entry.path() / "profile.json";
        if (entry.is_directory() && std::filesystem::exists(candidate)) files.push_back(candidate);
    }
    std::sort(files.begin(), files.end());  // порядок загрузки не зависит от файловой системы
    for (const auto& file : files)
        if (auto loaded = loadFile(file); !loaded) return loaded;
    return {};
}

std::vector<std::string> ProfileRegistry::ids() const {
    std::vector<std::string> out;
    for (const auto& [id, value] : raw_) out.push_back(id);
    return out;
}

Result<Profile> ProfileRegistry::resolve(const std::string& id) const {
    // Собираем цепочку наследования от профиля к его основе.
    std::vector<const json*> chain;
    std::set<std::string> seen;
    std::string current = id;
    while (!current.empty()) {
        if (!seen.insert(current).second)
            return makeError(ErrorCode::InvalidState, "циклическое наследование профилей: " + current);
        auto it = raw_.find(current);
        if (it == raw_.end()) return makeError(ErrorCode::NotFound, "профиль не найден: " + current);
        chain.push_back(&it->second);
        current = it->second.value("extends", std::string());
    }
    // Накладываем от основы к наследнику: наследник перекрывает отдельные поля.
    json merged = json::object();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        json layer = **it;
        // Добавляемые и отключаемые правила применяются к правилам основы.
        if (layer.contains("rulesAppend") || layer.contains("rulesDisable")) {
            json rules = layer.contains("rules") ? layer["rules"] : merged.value("rules", json::array());
            layer.erase("rules");
            std::set<std::string> disabled;
            for (const auto& d : layer.value("rulesDisable", json::array())) disabled.insert(d.get<std::string>());
            json kept = json::array();
            for (const auto& rule : rules)
                if (!disabled.count(rule.value("id", std::string()))) kept.push_back(rule);
            for (const auto& rule : layer.value("rulesAppend", json::array())) kept.push_back(rule);
            layer.erase("rulesAppend");
            layer.erase("rulesDisable");
            if (!layer.contains("rules")) layer["rules"] = kept;
        }
        merged.merge_patch(layer);
    }
    merged["id"] = id;
    merged.erase("extends");
    try {
        return profileFromJson(merged);
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, "профиль " + id + ": " + e.what());
    }
}

}  // namespace cosmocad::standards
