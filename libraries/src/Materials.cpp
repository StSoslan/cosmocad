#include "cosmocad/libraries/Materials.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

namespace cosmocad::libraries {

using core::ErrorCode;
using core::makeError;
using core::Result;
using nlohmann::json;

std::string lowered(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        } else if (c == 0xD0 && i + 1 < text.size()) {
            const auto d = static_cast<unsigned char>(text[i + 1]);
            ++i;
            if (d >= 0x90 && d <= 0x9F) {  // А…П → а…п
                out.push_back(static_cast<char>(0xD0));
                out.push_back(static_cast<char>(d + 0x20));
            } else if (d >= 0xA0 && d <= 0xAF) {  // Р…Я → р…я
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d - 0x20));
            } else if (d == 0x81) {  // Ё → ё
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(0x91));
            } else {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(d));
            }
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

namespace {

Result<std::vector<Material>> readItems(const json& j, bool user, const std::string& where) {
    std::vector<Material> out;
    for (const auto& item : j.at("items")) {
        Material m;
        m.name = item.at("name").get<std::string>();
        m.standard = item.value("standard", std::string());
        m.density = item.at("density").get<double>();
        m.note = item.value("note", std::string());
        m.user = user;
        if (m.name.empty()) return makeError(ErrorCode::ParseError, where + ": у материала нет названия");
        if (!(m.density > 0.0) || !std::isfinite(m.density) || m.density > 30.0)
            return makeError(ErrorCode::ParseError, where + ": плотность материала «" + m.name + "» должна быть больше 0 и не больше 30 г/см³");
        out.push_back(std::move(m));
    }
    return out;
}

Result<json> readFile(const std::filesystem::path& file, const std::string& where) {
    std::ifstream in(file);
    if (!in) return makeError(ErrorCode::IoError, where + ": не удалось открыть файл " + file.string());
    try {
        return json::parse(in);
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, where + ": " + e.what());
    }
}

json toJson(const std::vector<Material>& items) {
    json list = json::array();
    for (const auto& m : items) list.push_back({{"name", m.name}, {"standard", m.standard}, {"density", m.density}, {"note", m.note}});
    return {{"id", "user-materials"}, {"name", "Мои материалы"}, {"version", "1"}, {"units", "г/см³"}, {"items", std::move(list)}};
}

}  // namespace

Result<MaterialLibrary> MaterialLibrary::load(const std::filesystem::path& builtIn, const std::filesystem::path& userFile) {
    MaterialLibrary library;
    library.userFile_ = userFile;
    auto j = readFile(builtIn, "библиотека материалов");
    if (!j) return j.error();
    try {
        library.version_ = j.value().at("version").get<std::string>();
        library.source_ = j.value().value("source", std::string());
        auto items = readItems(j.value(), false, "библиотека материалов");
        if (!items) return items.error();
        library.builtIn_ = items.take();
        std::error_code ignored;
        if (!userFile.empty() && std::filesystem::exists(userFile, ignored)) {
            auto mine = readFile(userFile, "мои материалы");
            if (!mine) return mine.error();
            auto own = readItems(mine.value(), true, "мои материалы");
            if (!own) return own.error();
            library.user_ = own.take();
        }
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("библиотека материалов: ") + e.what());
    }
    return library;
}

std::vector<Material> MaterialLibrary::all() const {
    std::vector<Material> out;
    for (const auto& m : builtIn_)
        if (std::none_of(user_.begin(), user_.end(), [&](const Material& u) { return u.title() == m.title(); })) out.push_back(m);
    out.insert(out.end(), user_.begin(), user_.end());
    return out;
}

std::vector<Material> MaterialLibrary::find(const std::string& text) const {
    const std::string needle = lowered(text);
    std::vector<Material> out;
    for (const auto& m : all())
        if (needle.empty() || lowered(m.title()).find(needle) != std::string::npos) out.push_back(m);
    return out;
}

const Material* MaterialLibrary::byTitle(const std::string& title) const {
    for (const auto& m : user_)
        if (m.title() == title) return &m;
    for (const auto& m : builtIn_)
        if (m.title() == title) return &m;
    return nullptr;
}

Result<void> MaterialLibrary::saveUser() const {
    if (userFile_.empty()) return makeError(ErrorCode::InvalidState, "папка пользовательских библиотек не задана");
    std::error_code problem;
    std::filesystem::create_directories(userFile_.parent_path(), problem);
    std::ofstream out(userFile_);
    if (!out) return makeError(ErrorCode::IoError, "не удалось записать файл " + userFile_.string());
    out << toJson(user_).dump(1);
    return {};
}

Result<void> MaterialLibrary::addUser(const Material& material) {
    if (material.name.empty()) return makeError(ErrorCode::InvalidArgument, "у материала должно быть название");
    if (!(material.density > 0.0) || !std::isfinite(material.density) || material.density > 30.0)
        return makeError(ErrorCode::InvalidArgument, "плотность должна быть больше 0 и не больше 30 г/см³");
    Material own = material;
    own.user = true;
    const auto previous = user_;
    const auto it = std::find_if(user_.begin(), user_.end(), [&](const Material& m) { return m.title() == own.title(); });
    if (it != user_.end())
        *it = own;
    else
        user_.push_back(own);
    if (auto ok = saveUser(); !ok) {
        user_ = previous;
        return ok;
    }
    return {};
}

Result<void> MaterialLibrary::removeUser(const std::string& title) {
    const auto it = std::find_if(user_.begin(), user_.end(), [&](const Material& m) { return m.title() == title; });
    if (it == user_.end()) return makeError(ErrorCode::NotFound, "среди своих материалов нет «" + title + "»; встроенные материалы не удаляются");
    const auto previous = user_;
    user_.erase(it);
    if (auto ok = saveUser(); !ok) {
        user_ = previous;
        return ok;
    }
    return {};
}

Result<int> MaterialLibrary::exportUser(const std::filesystem::path& file) const {
    std::ofstream out(file);
    if (!out) return makeError(ErrorCode::IoError, "не удалось записать файл " + file.string());
    out << toJson(user_).dump(1);
    return static_cast<int>(user_.size());
}

Result<int> MaterialLibrary::importUser(const std::filesystem::path& file) {
    auto j = readFile(file, "импорт материалов");
    if (!j) return j.error();
    try {
        auto items = readItems(j.value(), true, "импорт материалов");
        if (!items) return items.error();
        const auto previous = user_;
        for (const auto& m : items.value()) {
            const auto it = std::find_if(user_.begin(), user_.end(), [&](const Material& u) { return u.title() == m.title(); });
            if (it != user_.end())
                *it = m;
            else
                user_.push_back(m);
        }
        if (auto ok = saveUser(); !ok) {
            user_ = previous;
            return ok.error();
        }
        return static_cast<int>(items.value().size());
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("импорт материалов: ") + e.what());
    }
}

}  // namespace cosmocad::libraries
