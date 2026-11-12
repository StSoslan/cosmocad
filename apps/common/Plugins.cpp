#include "Plugins.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "AppContext.h"

namespace cosmocad::app {

using core::ErrorCode;
using core::makeError;
using core::Result;
using nlohmann::json;

namespace {

// Тонкая оболочка над загрузкой библиотек операционной системы.
struct Library {
    void* handle = nullptr;
    std::string error;

    bool open(const std::filesystem::path& file) {
#if defined(_WIN32)
        handle = LoadLibraryW(file.wstring().c_str());
        if (!handle) error = "не удалось загрузить библиотеку " + file.string();
#else
        handle = dlopen(file.string().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) error = dlerror();
#endif
        return handle != nullptr;
    }
    void* symbol(const char* name) const {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }
    void close() {
        if (!handle) return;
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        handle = nullptr;
    }
};

const char* librarySuffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

}  // namespace

struct PluginManager::Loaded {
    std::string id;
    Library library;
    void (*shutdown)() = nullptr;
};

PluginManager::PluginManager(AppContext& context) : context_(context) {}

PluginManager::~PluginManager() {
    // Сначала уходят команды (их код лежит в библиотеках), потом дополнения завершаются и выгружаются.
    commands_.clear();
    for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
        try {
            if ((*it)->shutdown) (*it)->shutdown();
        } catch (...) {
        }
        (*it)->library.close();
    }
}

std::vector<const PluginCommand*> PluginManager::commands() const {
    std::vector<const PluginCommand*> out;
    for (const auto& command : commands_) out.push_back(&command);
    return out;
}

Result<std::string> PluginManager::run(const std::string& commandId, const json& args) {
    const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const PluginCommand& c) { return c.id == commandId; });
    if (it == commands_.end()) return makeError(ErrorCode::NotFound, "команда дополнения «" + commandId + "» не найдена");
    try {
        return it->run(context_, args);
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Internal, "дополнение завершило команду «" + commandId + "» с ошибкой: " + e.what());
    } catch (...) {
        return makeError(ErrorCode::Internal, "дополнение завершило команду «" + commandId + "» с неизвестной ошибкой");
    }
}

int PluginManager::loadFrom(const std::filesystem::path& directory, const std::vector<std::string>& disabled) {
    std::error_code ignored;
    if (!std::filesystem::is_directory(directory, ignored)) return 0;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ignored))
        if (entry.is_directory(ignored) && std::filesystem::exists(entry.path() / "plugin.json", ignored)) folders.push_back(entry.path());
    std::sort(folders.begin(), folders.end());  // порядок загрузки не зависит от файловой системы

    int active = 0;
    for (const auto& folder : folders) {
        PluginInfo info;
        info.directory = folder;
        info.id = folder.filename().string();
        std::string libraryName;
        int wantedApi = 0;
        const auto reject = [&](const std::string& reason) {
            info.state = PluginState::Rejected;
            info.reason = reason;
            plugins_.push_back(info);
        };
        try {
            std::ifstream in(folder / "plugin.json");
            const json manifest = json::parse(in);
            info.id = manifest.at("id").get<std::string>();
            info.name = manifest.value("name", info.id);
            info.version = manifest.value("version", std::string());
            info.description = manifest.value("description", std::string());
            wantedApi = manifest.at("apiVersion").get<int>();
            libraryName = manifest.at("library").get<std::string>();
        } catch (const json::exception& e) {
            reject(std::string("опись plugin.json не читается: ") + e.what());
            continue;
        }
        if (info.id.empty() || info.id.find('.') != std::string::npos) {
            reject("имя дополнения в описи пусто или содержит точку");
            continue;
        }
        if (std::any_of(plugins_.begin(), plugins_.end(), [&](const PluginInfo& p) { return p.id == info.id; })) {
            reject("дополнение с таким именем уже найдено в другой папке");
            continue;
        }
        if (wantedApi != kPluginApiVersion) {
            reject("дополнение собрано для версии интерфейса " + std::to_string(wantedApi) + ", а программа предлагает версию " +
                   std::to_string(kPluginApiVersion));
            continue;
        }
        if (std::find(disabled.begin(), disabled.end(), info.id) != disabled.end()) {
            info.state = PluginState::Disabled;
            info.reason = "выключено пользователем";
            plugins_.push_back(info);
            continue;
        }

        const auto fail = [&](const std::string& reason) {
            info.state = PluginState::Failed;
            info.reason = reason;
            plugins_.push_back(info);
        };
        auto loaded = std::make_unique<Loaded>();
        loaded->id = info.id;
        // Библиотека лежит в папке дополнения: имя из описи, с приставкой «lib» или без неё.
        std::filesystem::path file = folder / (libraryName + librarySuffix());
        if (!std::filesystem::exists(file, ignored)) file = folder / ("lib" + libraryName + librarySuffix());
        if (!std::filesystem::exists(file, ignored)) {
            fail("в папке дополнения нет библиотеки «" + libraryName + "»");
            continue;
        }
        if (!loaded->library.open(file)) {
            fail("библиотека не загрузилась: " + loaded->library.error);
            continue;
        }
        using VersionFn = int (*)();
        using InitializeFn = const char* (*)(IPluginHost*);
        using ShutdownFn = void (*)();
        const auto version = reinterpret_cast<VersionFn>(loaded->library.symbol("cosmocad_plugin_api_version"));
        const auto initialize = reinterpret_cast<InitializeFn>(loaded->library.symbol("cosmocad_plugin_initialize"));
        loaded->shutdown = reinterpret_cast<ShutdownFn>(loaded->library.symbol("cosmocad_plugin_shutdown"));
        if (!version || !initialize || !loaded->shutdown) {
            loaded->library.close();
            fail("в библиотеке нет обязательных функций дополнения");
            continue;
        }
        if (version() != kPluginApiVersion) {
            loaded->library.close();
            fail("библиотека собрана для другой версии интерфейса, чем указано в описи");
            continue;
        }

        // Команды собираются отдельно: при сбое инициализации ничего из них в программу не попадает.
        struct Host final : IPluginHost {
            Host(AppContext& c, const std::string& pluginId, const std::vector<PluginCommand>& existing)
                : context_(c), id_(pluginId), existing_(existing) {}
            AppContext& context() override { return context_; }
            Result<void> registerCommand(PluginCommand command) override {
                if (command.id.rfind(id_ + ".", 0) != 0 || command.id.size() == id_.size() + 1)
                    return makeError(ErrorCode::InvalidArgument, "имя команды должно начинаться с «" + id_ + ".»");
                if (!command.run) return makeError(ErrorCode::InvalidArgument, "у команды «" + command.id + "» нет действия");
                const auto same = [&](const PluginCommand& c) { return c.id == command.id; };
                if (std::any_of(existing_.begin(), existing_.end(), same) || std::any_of(added.begin(), added.end(), same))
                    return makeError(ErrorCode::InvalidState, "команда «" + command.id + "» уже есть");
                added.push_back(std::move(command));
                return {};
            }
            std::vector<PluginCommand> added;

        private:
            AppContext& context_;
            std::string id_;
            const std::vector<PluginCommand>& existing_;
        } host(context_, info.id, commands_);

        std::string problem;
        try {
            if (const char* error = initialize(&host)) problem = *error ? error : "инициализация не удалась";
        } catch (const std::exception& e) {
            problem = std::string("инициализация завершилась с ошибкой: ") + e.what();
        } catch (...) {
            problem = "инициализация завершилась с неизвестной ошибкой";
        }
        if (!problem.empty()) {
            host.added.clear();  // до выгрузки библиотеки: в командах — её код
            try {
                loaded->shutdown();
            } catch (...) {
            }
            loaded->library.close();
            fail(problem);
            continue;
        }
        for (auto& command : host.added) {
            info.commands.push_back(command.id);
            commands_.push_back(std::move(command));
        }
        info.state = PluginState::Active;
        plugins_.push_back(info);
        loaded_.push_back(std::move(loaded));
        ++active;
    }
    return active;
}

}  // namespace cosmocad::app
