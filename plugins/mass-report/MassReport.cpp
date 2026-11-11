// Пример дополнения CosmoCad: ведомость масс деталей проекта.
// Показывает всё, что нужно дополнению: три обязательные функции и регистрацию команды.
#include <fstream>
#include <sstream>

#include "AppContext.h"
#include "Plugins.h"

using namespace cosmocad;

namespace {

// Команда «mass-report.csv»: таблица «обозначение; название; материал; объём; масса».
// Поле args["path"] — файл для записи; без него таблица возвращается текстом.
core::Result<std::string> massReport(app::AppContext& context, const nlohmann::json& args) {
    std::ostringstream table;
    table << "Обозначение;Название;Материал;Объём, см3;Масса, г\n";
    int parts = 0;
    double total = 0.0;
    for (const auto& document : context.documents().project().documents()) {
        if (document->kind() != document::DocumentKind::Part) continue;
        if (auto rebuilt = context.modeling().recompute(*document); !rebuilt) return rebuilt.error();
        if (!context.modeling().body(*document)) continue;  // деталь без тела в ведомость не идёт
        auto mass = context.modeling().mass(*document);
        if (!mass) return mass.error();
        std::string designation, material;
        if (const auto properties = document->objectsOf<modeling::PartProperties>(); !properties.empty()) {
            designation = properties.front()->designation;
            material = properties.front()->material;
        }
        table.setf(std::ios::fixed);
        table.precision(2);
        table << designation << ';' << document->name << ';' << material << ';' << mass.value().geometry.volume / 1000.0 << ';'
              << mass.value().massGrams << '\n';
        total += mass.value().massGrams;
        ++parts;
    }
    table << ";Всего деталей: " << parts << ";;;" << total << '\n';
    if (args.contains("path") && args.at("path").is_string()) {
        const std::string path = args.at("path").get<std::string>();
        std::ofstream out(path, std::ios::binary);
        if (!out) return core::makeError(core::ErrorCode::IoError, "не удалось записать файл " + path);
        out << "\xEF\xBB\xBF" << table.str();  // метка UTF-8: файл открывается в Excel
        return "Ведомость масс записана: " + path + " (деталей: " + std::to_string(parts) + ")";
    }
    return table.str();
}

}  // namespace

#if defined(_WIN32)
#define COSMOCAD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define COSMOCAD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

COSMOCAD_PLUGIN_EXPORT int cosmocad_plugin_api_version() { return app::kPluginApiVersion; }

COSMOCAD_PLUGIN_EXPORT const char* cosmocad_plugin_initialize(app::IPluginHost* host) {
    auto registered = host->registerCommand({"mass-report.csv", "Ведомость масс",
                                             "Таблица объёмов и масс всех деталей проекта (CSV)", massReport});
    static std::string error;
    if (!registered) {
        error = registered.error().message;
        return error.c_str();
    }
    return nullptr;
}

COSMOCAD_PLUGIN_EXPORT void cosmocad_plugin_shutdown() {}

}  // extern "C"
