#include "cosmocad/storage/ProjectStorage.h"

#include <array>
#include <fstream>
#include <sstream>

#include "cosmocad/core/Log.h"
#include "cosmocad/core/Version.h"

namespace cosmocad::storage {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::makeError;
using core::Result;
using nlohmann::json;

std::uint32_t crc32(const std::string& data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace {

Result<void> writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "не удалось создать файл: " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) return makeError(ErrorCode::IoError, "ошибка записи файла: " + path.string());
    return {};
}

Result<std::string> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть файл: " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

Result<json> parseJson(const std::string& text, const std::string& what) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return makeError(ErrorCode::CorruptedData, what + ": файл повреждён");
    return j;
}

// Имя файла из описи не должно выводить за пределы каталога проекта.
bool safeRelativePath(const std::string& name) {
    fs::path p(name);
    if (name.empty() || p.is_absolute()) return false;
    for (const auto& part : p)
        if (part == "..") return false;
    return true;
}

// Если сохранение оборвалось между двумя переименованиями, прежняя версия лежит
// рядом под именем <имя>.bak-…; возвращаем её на место.
void recoverInterruptedSave(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    if (!fs::is_directory(parent, ec)) return;
    const std::string prefix = path.filename().string() + ".bak-";
    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            fs::rename(entry.path(), path, ec);
            if (!ec) core::logWarning("storage", "восстановлена предыдущая версия проекта после прерванного сохранения");
            return;
        }
    }
}

}  // namespace

ProjectStorage::ProjectStorage(document::ObjectFactory factory, int currentVersion)
    : factory_(std::move(factory)), currentVersion_(currentVersion) {}

void ProjectStorage::registerMigration(int fromVersion, Migration migration) {
    migrations_[fromVersion] = std::move(migration);
}

Result<void> ProjectStorage::writeContainer(const document::Project& project, const fs::path& directory) {
    std::error_code ec;
    fs::create_directories(directory / "documents", ec);
    if (ec) return makeError(ErrorCode::IoError, "не удалось создать каталог: " + directory.string());

    json manifest;
    manifest["format"] = "cosmocad";
    manifest["formatVersion"] = currentVersion_;
    manifest["appVersion"] = core::kAppVersion;
    manifest["projectId"] = project.id().toString();
    manifest["projectName"] = project.name;
    json documents = json::array();
    for (const auto& document : project.documents()) {
        std::string relative = "documents/" + document->id().toString() + ".json";
        std::string content = document->toJson().dump(1);
        if (auto written = writeFile(directory / relative, content); !written) return written;
        documents.push_back({{"id", document->id().toString()},
                             {"name", document->name},
                             {"kind", document::toString(document->kind())},
                             {"file", relative},
                             {"size", content.size()},
                             {"crc32", crc32(content)}});
    }
    manifest["documents"] = std::move(documents);
    return writeFile(directory / "manifest.json", manifest.dump(1));
}

Result<void> ProjectStorage::save(const document::Project& project, const fs::path& path) {
    std::error_code ec;
    const std::string suffix = core::Uuid::generate().toString();
    fs::path temporary = path;
    temporary += ".tmp-" + suffix;
    fs::path backup = path;
    backup += ".bak-" + suffix;

    if (auto written = writeContainer(project, temporary); !written) {
        fs::remove_all(temporary, ec);
        return written;
    }

    bool hadOld = fs::exists(path, ec);
    if (hadOld) {
        fs::rename(path, backup, ec);
        if (ec) {
            fs::remove_all(temporary, ec);
            return makeError(ErrorCode::IoError, "не удалось заменить прежнюю версию проекта: " + path.string());
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code restore;
        if (hadOld) fs::rename(backup, path, restore);
        fs::remove_all(temporary, restore);
        return makeError(ErrorCode::IoError, "не удалось записать проект: " + path.string());
    }
    if (hadOld) fs::remove_all(backup, ec);
    discardAutosave(path);
    return {};
}

Result<std::unique_ptr<document::Project>> ProjectStorage::load(const fs::path& path) {
    recoverInterruptedSave(path);

    std::error_code ec;
    if (!fs::is_directory(path, ec)) return makeError(ErrorCode::NotFound, "проект не найден: " + path.string());

    auto manifestText = readFile(path / "manifest.json");
    if (!manifestText) return manifestText.error();
    auto manifestJson = parseJson(manifestText.value(), "опись проекта");
    if (!manifestJson) return manifestJson.error();
    json manifest = manifestJson.take();

    if (!manifest.is_object() || manifest.value("format", std::string()) != "cosmocad" ||
        !manifest.contains("formatVersion") || !manifest["formatVersion"].is_number_integer())
        return makeError(ErrorCode::CorruptedData, "это не проект CosmoCad");

    int version = manifest["formatVersion"].get<int>();
    if (version > currentVersion_)
        return makeError(ErrorCode::UnsupportedVersion,
                         "проект сохранён более новой версией программы (формат " + std::to_string(version) +
                             ", поддерживается до " + std::to_string(currentVersion_) + ")");
    if (version < 1) return makeError(ErrorCode::CorruptedData, "недопустимая версия формата");

    // Читаем документы с проверкой контрольных сумм.
    std::map<std::string, json> documents;
    if (!manifest.contains("documents") || !manifest["documents"].is_array())
        return makeError(ErrorCode::CorruptedData, "в описи нет списка документов");
    for (const auto& entry : manifest["documents"]) {
        if (!entry.is_object() || !entry.contains("file") || !entry["file"].is_string())
            return makeError(ErrorCode::CorruptedData, "повреждена запись документа в описи");
        std::string file = entry["file"].get<std::string>();
        if (!safeRelativePath(file)) return makeError(ErrorCode::CorruptedData, "недопустимый путь в описи: " + file);
        auto text = readFile(path / file);
        if (!text) return text.error();
        if (entry.contains("crc32") && entry["crc32"].is_number_unsigned() &&
            entry["crc32"].get<std::uint32_t>() != crc32(text.value()))
            return makeError(ErrorCode::CorruptedData, "документ повреждён (не сходится контрольная сумма): " + file);
        auto parsed = parseJson(text.value(), file);
        if (!parsed) return parsed.error();
        documents[file] = parsed.take();
    }

    // Миграции: шаг за шагом до текущей версии.
    while (version < currentVersion_) {
        auto it = migrations_.find(version);
        if (it == migrations_.end())
            return makeError(ErrorCode::UnsupportedVersion,
                             "нет преобразования с версии формата " + std::to_string(version));
        if (auto migrated = it->second(manifest, documents); !migrated) return migrated.error();
        ++version;
    }

    auto project = std::make_unique<document::Project>(manifest.value("projectName", std::string("Без имени")));
    if (manifest.contains("projectId") && manifest["projectId"].is_string()) {
        auto id = core::Uuid::parse(manifest["projectId"].get<std::string>());
        if (!id) return id.error();
        project->restoreId(id.value());
    }
    // Порядок документов — как в описи.
    for (const auto& entry : manifest["documents"]) {
        auto it = documents.find(entry["file"].get<std::string>());
        if (it == documents.end()) continue;
        auto document = document::Document::fromJson(it->second, factory_);
        if (!document) return document.error();
        project->addDocument(document.take());
    }
    return project;
}

fs::path ProjectStorage::autosavePath(const fs::path& path) {
    fs::path result = path;
    result += ".autosave";
    return result;
}

Result<void> ProjectStorage::saveAutosave(const document::Project& project, const fs::path& path) {
    fs::path target = autosavePath(path);
    std::error_code ec;
    fs::path temporary = target;
    temporary += ".tmp-" + core::Uuid::generate().toString();
    if (auto written = writeContainer(project, temporary); !written) {
        fs::remove_all(temporary, ec);
        return written;
    }
    fs::remove_all(target, ec);
    fs::rename(temporary, target, ec);
    if (ec) return makeError(ErrorCode::IoError, "не удалось записать автосохранение");
    return {};
}

bool ProjectStorage::hasNewerAutosave(const fs::path& path) {
    std::error_code ec;
    fs::path manifest = autosavePath(path) / "manifest.json";
    if (!fs::exists(manifest, ec)) return false;
    fs::path original = path / "manifest.json";
    if (!fs::exists(original, ec)) return true;
    return fs::last_write_time(manifest, ec) > fs::last_write_time(original, ec);
}

void ProjectStorage::discardAutosave(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(autosavePath(path), ec);
}

}  // namespace cosmocad::storage
