// Хранилище проекта в формате .cosmocad (версия формата 1).
//
// Проект — каталог <имя>.cosmocad:
//   manifest.json            опись: версия формата, состав проекта, контрольные суммы
//   documents/<номер>.json   документы
// Упаковка каталога в один файл и база SQLite — следующий шаг, см. docs/architecture/data-format.md.
//
// Гарантии:
//   * сохранение безопасно: новая версия пишется рядом и только потом заменяет старую;
//   * повреждение файла обнаруживается при открытии по контрольной сумме;
//   * файл более новой версии формата не открывается молча с потерями;
//   * файлы старых версий преобразуются цепочкой миграций.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "cosmocad/document/IDocumentService.h"

namespace cosmocad::storage {

inline constexpr int kFormatVersion = 1;

std::uint32_t crc32(const std::string& data);

class ProjectStorage final : public document::IProjectStorage {
public:
    // Миграция переводит данные с версии N на N+1: получает опись и документы (по имени файла).
    using Migration =
        std::function<core::Result<void>(nlohmann::json& manifest, std::map<std::string, nlohmann::json>& documents)>;

    explicit ProjectStorage(document::ObjectFactory factory, int currentVersion = kFormatVersion);

    void registerMigration(int fromVersion, Migration migration);

    core::Result<void> save(const document::Project& project, const std::filesystem::path& path) override;
    core::Result<std::unique_ptr<document::Project>> load(const std::filesystem::path& path) override;

    // --- Автосохранение для восстановления после сбоя ---
    static std::filesystem::path autosavePath(const std::filesystem::path& path);
    core::Result<void> saveAutosave(const document::Project& project, const std::filesystem::path& path);
    static bool hasNewerAutosave(const std::filesystem::path& path);
    static void discardAutosave(const std::filesystem::path& path);

private:
    core::Result<void> writeContainer(const document::Project& project, const std::filesystem::path& directory);

    document::ObjectFactory factory_;
    int currentVersion_;
    std::map<int, Migration> migrations_;
};

}  // namespace cosmocad::storage
