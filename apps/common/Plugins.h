// Дополнения (плагины): отдельные библиотеки, которые добавляют программе команды.
//
// Дополнение — папка с описью plugin.json и библиотекой. В библиотеке три функции:
//   int         cosmocad_plugin_api_version();                 // для какой версии интерфейса собрано
//   const char* cosmocad_plugin_initialize(IPluginHost* host);  // nullptr — успех, иначе текст ошибки
//   void        cosmocad_plugin_shutdown();
// При инициализации дополнение регистрирует свои команды через IPluginHost. Команду можно вызвать
// из окна (вкладка «Дополнения»), из cosmocad-cli и из пакетного режима.
//
// Сбой дополнения не роняет программу: причина записывается в его состояние, программа работает без него.
// Дополнение должно быть собрано тем же компилятором и из той же версии исходников, что программа:
// это проверяется по номеру версии интерфейса. Дополнения с ИИ запрещены так же, как ИИ в самой программе.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cosmocad/core/Result.h"

namespace cosmocad::app {

class AppContext;

inline constexpr int kPluginApiVersion = 1;

// Команда дополнения. args — поля, переданные при вызове; ответ — текст для пользователя.
struct PluginCommand {
    std::string id;           // «mass-report.csv»: имя дополнения, точка, имя команды
    std::string title;        // название для кнопки
    std::string description;  // что делает
    std::function<core::Result<std::string>(AppContext&, const nlohmann::json& args)> run;
};

// Что программа даёт дополнению при инициализации.
class IPluginHost {
public:
    virtual ~IPluginHost() = default;
    virtual AppContext& context() = 0;
    // Идентификатор команды должен начинаться с имени дополнения и быть свободным.
    virtual core::Result<void> registerCommand(PluginCommand command) = 0;
};

enum class PluginState {
    Found,     // опись прочитана
    Rejected,  // опись неверна или версия интерфейса не подходит
    Disabled,  // выключено пользователем
    Failed,    // библиотека не загрузилась или инициализация вернула ошибку
    Active     // работает
};

struct PluginInfo {
    std::string id, name, version, description;
    std::filesystem::path directory;
    PluginState state = PluginState::Found;
    std::string reason;                 // почему отклонено или не загрузилось
    std::vector<std::string> commands;  // идентификаторы зарегистрированных команд
};

class PluginManager {
public:
    explicit PluginManager(AppContext& context);
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Ищет дополнения в папке (каждое — в своей подпапке с plugin.json) и загружает подходящие.
    // disabled — имена дополнений, выключенных пользователем. Возвращает число активных из этой папки.
    int loadFrom(const std::filesystem::path& directory, const std::vector<std::string>& disabled = {});

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    std::vector<const PluginCommand*> commands() const;
    // Выполняет команду дополнения. Исключение внутри дополнения превращается в ошибку.
    core::Result<std::string> run(const std::string& commandId, const nlohmann::json& args = nlohmann::json::object());

private:
    struct Loaded;
    AppContext& context_;
    std::vector<PluginInfo> plugins_;
    std::vector<std::unique_ptr<Loaded>> loaded_;
    std::vector<PluginCommand> commands_;
};

}  // namespace cosmocad::app
