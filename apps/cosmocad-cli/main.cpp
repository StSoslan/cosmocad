// Консольная утилита CosmoCad: работа с проектами без окон.
// Нужна для пакетной проверки оформления и автоматических тестов.
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
#include <cstddef>
#include <exception>
#include <utility>
#include <algorithm>
#include <optional>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <string>
#include <vector>

#include "AppContext.h"
#include "Batch.h"
#include "cosmocad/core/Version.h"
#include "cosmocad/document/Transaction.h"
#include "cosmocad/exchange/Exchange.h"
#include "cosmocad/graphics/Scene.h"

namespace fs = std::filesystem;
using namespace cosmocad;

namespace {

int usage() {
    std::cout << core::kAppName << " " << core::kAppVersion << " — консольная утилита\n\n"
              << "Команды:\n"
              << "  cosmocad-cli demo <проект.cosmocad>                создать проект с примером чертежа\n"
              << "  cosmocad-cli check <проект.cosmocad>               проверить оформление по профилю\n"
              << "  cosmocad-cli export <проект.cosmocad> <файл>       экспорт первого чертежа (.svg или .dxf)\n"
              << "  cosmocad-cli import-dxf <файл.dxf> <проект.cosmocad>  создать проект из DXF\n"
              << "  cosmocad-cli profiles                               список профилей стандартов\n"
              << "  cosmocad-cli batch <проект.cosmocad> <команды.json>   выполнить список команд (основа скриптов Python)\n"
              << "  cosmocad-cli plugins                                список дополнений и их команд\n"
              << "  cosmocad-cli plugin <команда> <проект.cosmocad> [имя=значение …]   выполнить команду дополнения\n"
              << "  cosmocad-cli library                                библиотека крепежа: что в ней есть\n"
              << "  cosmocad-cli pipes                                  трубы из полипропилена: материалы, классы, размеры\n"
              << "  cosmocad-cli pipe <материал> <класс> <диаметр> <SDR или стенка=мм> <давление, МПа>\n"
              << "                                                      расчёт трубы: напряжение, допустимое давление, срок службы\n"
              << "                                                      пример: cosmocad-cli pipe PP-R 1 25 6 0.6\n\n"
              << "3D:\n"
              << "  cosmocad-cli demo3d <проект.cosmocad>              пример: две детали, сборка, чертёж с видами\n"
              << "  cosmocad-cli model <проект.cosmocad>               состояние деталей и сборок проекта\n"
              << "  cosmocad-cli set <проект> <деталь> <параметр> <формула>\n"
              << "                                                      изменить параметр, перестроить модель,\n"
              << "                                                      сборки и виды на чертежах, сохранить\n"
              << "  cosmocad-cli export-step <проект> <документ> <файл.step>  деталь или сборка в STEP\n"
              << "  cosmocad-cli export-stl <проект> <документ> <файл.stl>    деталь или сборка в STL\n"
              << "  cosmocad-cli bom <проект> <сборка> <файл.csv>             состав изделия в таблицу CSV\n\n"
              << "Электросхемы:\n"
              << "  cosmocad-cli demo-scheme <проект.cosmocad>         пример схемы\n"
              << "  cosmocad-cli scheme <проект.cosmocad>              цепи, перечень элементов и проверка схем проекта\n"
              << "  cosmocad-cli scheme-list <проект> <лист> <файл.csv>  перечень элементов в таблицу CSV\n\n"
              << "Код возврата: 0 — успех, 1 — найдены ошибки оформления или сбои построения, 2 — сбой команды.\n";
    return 2;
}

int fail(const core::Error& error) {
    std::cerr << "Ошибка: " << error.message << " [" << core::toString(error.code) << "]\n";
    return 2;
}

document::Document* firstDrawing(app::AppContext& context) {
    for (const auto& document : context.documents().project().documents())
        if (document->kind() == document::DocumentKind::Drawing) return document.get();
    return nullptr;
}

document::Document* documentNamed(app::AppContext& context, const std::string& name) {
    for (const auto& document : context.documents().project().documents())
        if (document->name == name) return document.get();
    return nullptr;
}

// Печатает состояние деталей и сборок. Возвращает число сбоев построения и нерешённых сопряжений.
core::Result<int> printModels(app::AppContext& context) {
    int problems = 0;
    for (const auto& document : context.documents().project().documents()) {
        if (document->kind() == document::DocumentKind::Part) {
            std::cout << "Деталь «" << document->name << "»\n";
            auto values = context.modeling().parameterValues(*document);
            if (!values) return values.error();
            for (const auto* parameter : document->objectsOf<modeling::Parameter>())
                std::cout << "  параметр " << parameter->name << " = " << parameter->expression << " → "
                          << values.value().at(parameter->name) << "\n";
            const auto statuses = context.modeling().status(*document);
            std::size_t index = 0;
            for (const auto* feature : context.modeling().features(*document)) {
                const auto& status = statuses.at(index++);
                const char* state = status.state == modeling::FeatureState::UpToDate    ? "построено"
                                    : status.state == modeling::FeatureState::Suppressed ? "отключено"
                                                                                         : "СБОЙ";
                std::cout << "  " << feature->name << " = " << feature->value << ": " << state;
                if (status.state == modeling::FeatureState::Failed) {
                    std::cout << " — " << status.diagnostic;
                    ++problems;
                }
                std::cout << "\n";
            }
            if (auto mass = context.modeling().mass(*document))
                std::cout << "  объём " << mass.value().geometry.volume / 1000.0 << " см³, масса "
                          << mass.value().massGrams << " г\n";
            else
                std::cout << "  тела нет\n";
        } else if (document->kind() == document::DocumentKind::Assembly) {
            std::cout << "Сборка «" << document->name << "»\n";
            auto solved = context.assembly().solve(*document);
            if (!solved) return solved.error();
            std::cout << "  сопряжений: " << document->objectsOf<assembly::Mate>().size() << ", "
                      << (solved.value().solved ? "все выполнены" : "ЕСТЬ НЕВЫПОЛНЕННЫЕ") << "\n";
            for (const auto& [id, message] : solved.value().conflicts) {
                (void)id;
                std::cout << "    " << message << "\n";
                ++problems;
            }
            auto hits = context.assembly().checkInterference(*document);
            if (!hits) return hits.error();
            std::cout << "  пересечений компонентов: " << hits.value().size() << "\n";
            auto bom = context.assembly().billOfMaterials(*document);
            if (!bom) return bom.error();
            for (const auto& line : bom.value())
                std::cout << "  " << line.designation << "  " << line.name << " — " << line.quantity << " шт., "
                          << line.massGrams << " г/шт.\n";
        }
    }
    return problems;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Русский текст в окне командной строки Windows.
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

    std::error_code ec;
    fs::path executableDirectory = fs::weakly_canonical(fs::path(argv[0]), ec).parent_path();
    auto created = app::AppContext::create(app::findResourceDirectory(executableDirectory));
    if (!created) return fail(created.error());
    auto context = created.take();
    context->loadPlugins(executableDirectory);
    const std::string& command = args[0];

    if (command == "batch" && args.size() == 3) {
        // Проект открывается, если файл есть, иначе создаётся новый. Сохраняется только при полном успехе.
        std::error_code ignored;
        if (fs::exists(args[1], ignored)) {
            if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
            if (auto rebuilt = context->rebuildAll(); !rebuilt) return fail(rebuilt.error());
        } else {
            context->documents().newProject(fs::path(args[1]).stem().string());
        }
        std::ifstream in(args[2]);
        if (!in) return fail(core::makeError(core::ErrorCode::IoError, "не удалось открыть файл команд " + args[2]));
        nlohmann::json commands;
        try {
            commands = nlohmann::json::parse(in);
        } catch (const nlohmann::json::exception& e) {
            return fail(core::makeError(core::ErrorCode::ParseError, std::string("файл команд: ") + e.what()));
        }
        auto replies = app::runBatch(*context, commands);
        if (!replies) return fail(replies.error());
        if (auto saved = context->documents().saveProject(args[1]); !saved) return fail(saved.error());
        std::cout << replies.value().dump(1) << "\n";
        return 0;
    }

    if (command == "plugins") {
        static const std::map<app::PluginState, const char*> states = {{app::PluginState::Found, "найдено"},
                                                                     {app::PluginState::Rejected, "отклонено"},
                                                                     {app::PluginState::Disabled, "выключено"},
                                                                     {app::PluginState::Failed, "сбой"},
                                                                     {app::PluginState::Active, "работает"}};
        if (context->plugins().plugins().empty()) std::cout << "Дополнений не найдено.\n";
        for (const auto& plugin : context->plugins().plugins()) {
            std::cout << plugin.id << "  " << plugin.version << "  " << plugin.name << " — " << states.at(plugin.state);
            if (!plugin.reason.empty()) std::cout << ": " << plugin.reason;
            std::cout << "\n";
            for (const auto& id : plugin.commands) std::cout << "    команда " << id << "\n";
        }
        return 0;
    }

    if (command == "plugin" && args.size() >= 3) {
        if (auto opened = context->documents().openProject(args[2]); !opened) return fail(opened.error());
        if (auto rebuilt = context->rebuildAll(); !rebuilt) return fail(rebuilt.error());
        nlohmann::json fields = nlohmann::json::object();
        for (std::size_t i = 3; i < args.size(); ++i) {
            const auto split = args[i].find('=');
            if (split == std::string::npos) return fail(core::makeError(core::ErrorCode::InvalidArgument, "поле команды пишется как имя=значение: " + args[i]));
            fields[args[i].substr(0, split)] = args[i].substr(split + 1);
        }
        auto result = context->plugins().run(args[1], fields);
        if (!result) return fail(result.error());
        std::cout << result.value() << "\n";
        return 0;
    }

    if (command == "pipes") {
        const auto& pipes = context->pipes();
        std::cout << "Трубы из полипропилена, " << pipes.standard() << "\nИсточник: " << pipes.source() << "\n\nМатериалы:\n";
        for (const auto& material : pipes.materials()) std::cout << "  " << material.name << " — " << material.title << "\n";
        std::cout << "Классы эксплуатации и допустимое напряжение на " << pipes.designLifeYears() << " лет, МПа:\n";
        for (const auto& c : pipes.classes()) {
            std::cout << "  " << c.name << " — " << c.title << ":";
            for (const auto& material : pipes.materials())
                std::cout << "  " << material.name << " " << pipes.designStress(material.name, c.name).value();
            std::cout << "\n";
        }
        std::cout << "Толщина стенки, мм (диаметр: по сериям SDR";
        for (const double sdr : pipes.sdrs()) std::cout << " " << sdr;
        std::cout << "):\n";
        for (const double d : pipes.diameters()) {
            std::cout << "  " << d << ":";
            for (const double sdr : pipes.sdrs()) std::cout << " " << pipes.wall(d, sdr).value();
            std::cout << "\n";
        }
        return 0;
    }

    if (command == "pipe" && args.size() == 6) {
        const auto& pipes = context->pipes();
        const auto numberOf = [](std::string text) -> std::optional<double> {
            std::replace(text.begin(), text.end(), ',', '.');
            try {
                std::size_t used = 0;
                const double value = std::stod(text, &used);
                if (used != text.size()) return std::nullopt;
                return value;
            } catch (const std::exception&) {
                return std::nullopt;
            }
        };
        const auto diameter = numberOf(args[3]);
        const auto pressure = numberOf(args[5]);
        std::optional<double> wall;
        const std::string prefix = "стенка=";
        if (args[4].rfind(prefix, 0) == 0) {
            wall = numberOf(args[4].substr(prefix.size()));
        } else if (const auto sdr = numberOf(args[4]); sdr && diameter) {
            auto found = pipes.wall(*diameter, *sdr);
            if (!found) return fail(found.error());
            wall = found.value();
        }
        if (!diameter || !pressure || !wall) {
            std::cerr << "Диаметр, серия (или стенка=мм) и давление должны быть числами.\n";
            return 2;
        }
        auto report = pipes.check(args[1], args[2], *diameter, *wall, *pressure);
        if (!report) return fail(report.error());
        for (const auto& line : report.value().lines) std::cout << line << "\n";
        return report.value().ok ? 0 : 1;
    }

    if (command == "library") {
        const auto& library = context->fasteners();
        std::cout << "Библиотека крепежа, версия " << library.version() << "\nИсточник: " << library.source() << "\n";
        for (const auto& bolt : library.bolts()) {
            auto info = library.describe({libraries::FastenerKind::Bolt, bolt.d, library.boltLengths(bolt.d).front()});
            std::cout << "  Болт М" << bolt.d << ": под ключ " << bolt.S << ", головка " << bolt.k << ", длины "
                      << bolt.lengthMin << "–" << bolt.lengthMax << " мм (" << library.boltLengths(bolt.d).size() << " шт.)"
                      << (bolt.notRecommended ? ", не рекомендуется" : "") << "\n";
            if (!info) return fail(info.error());
        }
        for (const auto& nut : library.nuts())
            std::cout << "  Гайка М" << nut.d << ": под ключ " << nut.S << ", высота " << nut.m << "\n";
        for (const auto& washer : library.washers())
            std::cout << "  Шайба " << washer.d << ": " << washer.d1 << " × " << washer.d2 << " × " << washer.s << "\n";
        return 0;
    }

    if (command == "profiles") {
        for (const auto& id : context->standards().profileIds()) {
            auto profile = context->standards().profile(id);
            if (!profile) return fail(profile.error());
            std::cout << id << "  версия " << profile.value().version << "  " << profile.value().name
                      << "  (правил: " << profile.value().rules.size() << ")\n";
        }
        return 0;
    }

    if (command == "demo" && args.size() == 2) {
        context->documents().newProject("Пример");
        if (auto demo = context->createDemoDrawing(); !demo) return fail(demo.error());
        if (auto saved = context->documents().saveProject(args[1]); !saved) return fail(saved.error());
        std::cout << "Проект создан: " << args[1] << "\n";
        return 0;
    }

    if (command == "demo3d" && args.size() == 2) {
        context->documents().newProject("Пример 3D");
        if (auto demo = context->createDemoModel(); !demo) return fail(demo.error());
        if (auto saved = context->documents().saveProject(args[1]); !saved) return fail(saved.error());
        std::cout << "Проект создан: " << args[1] << "\n";
        return 0;
    }

    if (command == "model" && args.size() == 2) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        if (auto rebuilt = context->rebuildAll(); !rebuilt) return fail(rebuilt.error());
        auto problems = printModels(*context);
        if (!problems) return fail(problems.error());
        return problems.value() > 0 ? 1 : 0;
    }

    if (command == "set" && args.size() == 5) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        document::Document* part = documentNamed(*context, args[2]);
        if (!part) return fail(core::makeError(core::ErrorCode::NotFound, "в проекте нет документа «" + args[2] + "»"));
        if (auto changed = context->modeling().setParameter(*part, args[3], args[4]); !changed) return fail(changed.error());
        if (auto rebuilt = context->rebuildAll(); !rebuilt) return fail(rebuilt.error());
        int views = 0, movedDimensions = 0, lostDimensions = 0;
        for (const auto& document : context->documents().project().documents()) {
            if (document->kind() == document::DocumentKind::Assembly)
                if (auto solved = context->assembly().solve(*document); !solved) return fail(solved.error());
            if (document->kind() == document::DocumentKind::Drawing) {
                auto updated = context->updateModelViews(*document);
                if (!updated) return fail(updated.error());
                views += updated.value();
                movedDimensions += context->lastViewUpdate().movedDimensions;
                lostDimensions += context->lastViewUpdate().lostDimensions;
            }
        }
        auto problems = printModels(*context);
        if (!problems) return fail(problems.error());
        if (auto saved = context->documents().saveProject(args[1]); !saved) return fail(saved.error());
        std::cout << "Параметр изменён, обновлено видов на чертежах: " << views << ", размеров переехало: " << movedDimensions
                  << ", потеряли привязку: " << lostDimensions << ". Проект сохранён.\n";
        return problems.value() > 0 ? 1 : 0;
    }

    if ((command == "export-step" || command == "export-stl") && args.size() == 4) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        document::Document* document = documentNamed(*context, args[2]);
        if (!document) return fail(core::makeError(core::ErrorCode::NotFound, "в проекте нет документа «" + args[2] + "»"));
        auto bodies = context->bodiesOf(*document);
        if (!bodies) return fail(bodies.error());
        if (command == "export-step") {
            if (auto saved = context->kernel().exportStep(bodies.value(), args[3]); !saved) return fail(saved.error());
        } else {
            auto whole = context->kernel().combine(bodies.value());
            if (!whole) return fail(whole.error());
            if (auto saved = context->kernel().exportStl(whole.value(), args[3], 0.05); !saved) return fail(saved.error());
        }
        std::cout << "Экспортировано: " << args[3] << " (тел: " << bodies.value().size() << ")\n";
        return 0;
    }

    if (command == "demo-scheme" && args.size() == 2) {
        context->documents().newProject("Пример схемы");
        if (auto demo = context->createDemoSchematic(); !demo) return fail(demo.error());
        if (auto saved = context->documents().saveProject(args[1]); !saved) return fail(saved.error());
        std::cout << "Проект создан: " << args[1] << "\n";
        return 0;
    }

    if (command == "scheme" && args.size() == 2) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        int problems = 0;
        for (const auto& document : context->documents().project().documents()) {
            if (document->objectsOf<schematic::Element>().empty() && document->objectsOf<schematic::Wire>().empty()) continue;
            std::cout << "Схема «" << document->name << "»\n";
            for (const auto& net : context->schematic().nets(*document)) {
                std::cout << "  цепь " << net.number;
                if (!net.name.empty()) std::cout << " «" << net.name << "»";
                std::cout << ":";
                for (const auto& pin : net.pins) std::cout << " " << pin.designator << ":" << pin.pin;
                std::cout << "\n";
            }
            for (const auto& row : context->schematic().elementList(*document))
                std::cout << "  " << row.designators << " — " << row.name << ", " << row.count << " шт.\n";
            const auto issues = context->schematic().check(*document);
            std::cout << "  замечаний по схеме: " << issues.size() << "\n";
            for (const auto& issue : issues) std::cout << "    " << issue.message << "\n";
            problems += static_cast<int>(issues.size());
        }
        return problems > 0 ? 1 : 0;
    }

    if (command == "scheme-list" && args.size() == 4) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        document::Document* document = documentNamed(*context, args[2]);
        if (!document) return fail(core::makeError(core::ErrorCode::NotFound, "в проекте нет документа «" + args[2] + "»"));
        if (auto saved = context->schematic().exportElementListCsv(*document, args[3]); !saved) return fail(saved.error());
        std::cout << "Перечень элементов записан: " << args[3] << "\n";
        return 0;
    }

    if (command == "bom" && args.size() == 4) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        document::Document* document = documentNamed(*context, args[2]);
        if (!document) return fail(core::makeError(core::ErrorCode::NotFound, "в проекте нет документа «" + args[2] + "»"));
        if (auto saved = context->exportBillOfMaterialsCsv(*document, args[3]); !saved) return fail(saved.error());
        std::cout << "Состав изделия записан: " << args[3] << "\n";
        return 0;
    }

    if (command == "check" && args.size() == 2) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        bool hasErrors = false;
        for (const auto& document : context->documents().project().documents()) {
            // Оформление проверяется у чертежей; детали и сборки проверяет команда model.
            if (document->kind() != document::DocumentKind::Drawing) continue;
            auto violations = context->standards().check(*document);
            if (!violations) return fail(violations.error());
            std::cout << "Документ «" << document->name << "», профиль " << document->profileId << ": замечаний "
                      << violations.value().size() << "\n";
            for (const auto& v : violations.value()) {
                hasErrors = hasErrors || v.severity == standards::Severity::Error;
                std::cout << "  [" << standards::toString(v.severity) << "] " << v.ruleId << ": " << v.message;
                if (!v.reference.empty()) std::cout << " — " << v.reference;
                if (!v.objectId.isNil()) std::cout << " (объект " << v.objectId.toString() << ")";
                std::cout << "\n";
            }
            if (auto stale = context->staleViewCount(*document); stale && stale.value() > 0)
                std::cout << "  виды по модели устарели: " << stale.value() << " — модель изменилась после построения видов\n";
        }
        return hasErrors ? 1 : 0;
    }

    if (command == "export" && args.size() == 3) {
        if (auto opened = context->documents().openProject(args[1]); !opened) return fail(opened.error());
        document::Document* document = firstDrawing(*context);
        if (!document) return fail(core::makeError(core::ErrorCode::NotFound, "в проекте нет чертежа"));
        auto profile = context->profileFor(*document);
        graphics::Scene scene = graphics::buildScene(*document, profile ? &*profile : nullptr);

        fs::path target = args[2];
        std::ofstream out(target, std::ios::binary);
        if (!out) return fail(core::makeError(core::ErrorCode::IoError, "не удалось создать файл " + target.string()));
        std::string extension = target.extension().string();
        core::Result<void> written = core::makeError(core::ErrorCode::InvalidArgument,
                                                     "неизвестное расширение: поддерживаются .svg и .dxf");
        if (extension == ".svg") written = exchange::writeSvg(scene, out);
        if (extension == ".dxf") written = exchange::writeDxf(scene, out);
        if (!written) return fail(written.error());
        std::cout << "Экспортировано: " << target.string() << " (элементов: " << scene.items.size() << ")\n";
        return 0;
    }

    if (command == "import-dxf" && args.size() == 3) {
        std::ifstream in(args[1], std::ios::binary);
        if (!in) return fail(core::makeError(core::ErrorCode::IoError, "не удалось открыть " + args[1]));
        auto imported = exchange::readDxf(in);
        if (!imported) return fail(imported.error());

        context->documents().newProject(fs::path(args[1]).stem().string());
        auto document = context->documents().createDocument(document::DocumentKind::Drawing,
                                                            fs::path(args[1]).stem().string());
        if (!document) return fail(document.error());
        std::size_t count = imported.value().objects.size();
        {
            document::Transaction tx(*document.value(), "Импорт DXF");
            for (auto& object : imported.value().objects)
                if (auto added = tx.add(std::move(object)); !added) return fail(added.error());
            tx.commit();
        }
        if (auto saved = context->documents().saveProject(args[2]); !saved) return fail(saved.error());
        std::cout << "Импортировано объектов: " << count << ", пропущено элементов неподдерживаемых типов: "
                  << imported.value().skippedEntities << "\n";
        return 0;
    }

    return usage();
}
