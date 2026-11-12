#include "AppContext.h"

#include "cosmocad/document/Transaction.h"
#include "cosmocad/geometry/OcctKernel.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace cosmocad::app {

namespace fs = std::filesystem;
using core::Result;
using geometry::Point2;

fs::path findResourceDirectory(const fs::path& executableDirectory) {
    std::error_code ec;
    if (const char* env = std::getenv("COSMOCAD_RESOURCES"); env && fs::is_directory(env, ec)) return env;
    for (const fs::path& candidate :
         {executableDirectory / "resources", executableDirectory / ".." / "share" / "cosmocad" / "resources"})
        if (fs::is_directory(candidate / "standards", ec)) return candidate;
#ifdef COSMOCAD_SOURCE_RESOURCES
    if (fs::is_directory(COSMOCAD_SOURCE_RESOURCES, ec)) return COSMOCAD_SOURCE_RESOURCES;
#endif
    return executableDirectory / "resources";
}

Result<std::unique_ptr<AppContext>> AppContext::create(const fs::path& resourceDirectory,
                                                       const std::optional<fs::path>& userProfileDirectory) {
    standards::ProfileRegistry registry;
    if (auto loaded = registry.loadDirectory(resourceDirectory / "standards"); !loaded) return loaded.error();
    if (userProfileDirectory)
        if (auto loaded = registry.loadDirectory(*userProfileDirectory); !loaded) return loaded.error();

    document::ObjectFactory factory;
    drawing::registerTypes(factory);
    modeling::registerTypes(factory);
    schematic::registerTypes(factory);
    sketcher::registerTypes(factory);
    assembly::registerTypes(factory);

    std::unique_ptr<AppContext> context(new AppContext());
    context->storage_ = std::make_unique<storage::ProjectStorage>(std::move(factory));
    context->documents_ = std::make_unique<document::DocumentService>(*context->storage_);
    context->standards_ = std::make_unique<standards::StandardsService>(std::move(registry));
    // Набор условных обозначений схем лежит рядом с профилем ЕСКД.
    auto symbols = schematic::SymbolLibrary::load(resourceDirectory / "standards" / "eskd" / "symbols.json");
    if (!symbols) return symbols.error();
    context->schematic_ = std::make_unique<schematic::SchematicService>(symbols.take());
    auto fasteners = libraries::FastenerLibrary::load(resourceDirectory / "libraries" / "fasteners.json");
    if (!fasteners) return fasteners.error();
    context->fasteners_ = std::make_unique<libraries::FastenerLibrary>(fasteners.take());
    // Папка пользователя: переменная COSMOCAD_USER_DIR, иначе обычное место для настроек в системе.
    std::filesystem::path userDirectory;
    if (const char* custom = std::getenv("COSMOCAD_USER_DIR"); custom && *custom)
        userDirectory = custom;
    else if (const char* appData = std::getenv("APPDATA"); appData && *appData)
        userDirectory = std::filesystem::path(appData) / "CosmoCad";
    else if (const char* home = std::getenv("HOME"); home && *home)
        userDirectory = std::filesystem::path(home) / ".config" / "cosmocad";
    auto materials = libraries::MaterialLibrary::load(resourceDirectory / "libraries" / "materials.json",
                                                      userDirectory.empty() ? std::filesystem::path() : userDirectory / "materials.json");
    if (!materials) return materials.error();
    context->materials_ = std::make_unique<libraries::MaterialLibrary>(materials.take());
    auto pipes = piping::PipeLibrary::load(resourceDirectory / "libraries" / "pipes_pp.json");
    if (!pipes) return pipes.error();
    context->pipes_ = std::make_unique<piping::PipeLibrary>(pipes.take());
    auto profiles = libraries::ProfileLibrary::load(resourceDirectory / "libraries" / "profiles.json");
    if (!profiles) return profiles.error();
    context->profiles_ = std::make_unique<libraries::ProfileLibrary>(profiles.take());
    context->userDirectory_ = userDirectory;
    context->plugins_ = std::make_unique<PluginManager>(*context);
    context->kernel_ = geometry::createOcctKernel();
    context->modeling_ = std::make_unique<modeling::ModelingService>(*context->kernel_);
    context->partSource_ = std::make_unique<PartSource>(*context);
    context->assembly_ = std::make_unique<assembly::AssemblyService>(*context->kernel_, *context->partSource_);
    return context;
}

std::optional<standards::Profile> AppContext::profileFor(const document::Document& document) const {
    if (document.profileId.empty()) return std::nullopt;
    auto profile = standards_->profile(document.profileId);
    if (!profile) return std::nullopt;
    return profile.take();
}

Result<document::Document*> AppContext::createDemoDrawing() {
    auto eskd = standards_->profile("eskd");
    if (!eskd) return eskd.error();

    auto created = documents_->createDocument(document::DocumentKind::Drawing, "Пластина");
    if (!created) return created;
    document::Document& doc = *created.value();

    drawing::SheetSetup setup;
    setup.profileId = "eskd";
    setup.profileVersion = eskd.value().version;
    setup.format = "A4";
    if (auto sheet = drawing().setupSheet(doc, setup); !sheet) return sheet.error();
    for (const auto& [field, value] : {std::pair{"designation", "КСМ.001.001"},
                                       std::pair{"title", "Пластина"},
                                       std::pair{"developer", "Иванов"},
                                       std::pair{"organization", "CosmoCad"},
                                       std::pair{"material", "Сталь 45 ГОСТ 1050-2013"},
                                       std::pair{"scale", "1:1"},
                                       std::pair{"sheetCount", "1"}})
        if (auto set = drawing().setTitleBlockField(doc, field, value); !set) return set.error();

    auto contour = drawing().addLayer(doc, "Контур", "solid_main");
    auto axes = drawing().addLayer(doc, "Оси", "dash_dot");
    auto dimensions = drawing().addLayer(doc, "Размеры", "solid_thin");
    if (!contour) return contour.error();
    if (!axes) return axes.error();
    if (!dimensions) return dimensions.error();

    // Пластина 100 × 60 мм с отверстием ⌀30 и скруглённым углом.
    const Point2 o{55.0, 170.0};
    const double w = 100.0, h = 60.0, r = 10.0;
    struct Segment {
        Point2 a, b;
    };
    for (const Segment& s : {Segment{{o.x, o.y}, {o.x + w, o.y}}, Segment{{o.x + w, o.y}, {o.x + w, o.y + h - r}},
                             Segment{{o.x + w - r, o.y + h}, {o.x, o.y + h}}, Segment{{o.x, o.y + h}, {o.x, o.y}}})
        if (auto line = drawing().addLine(doc, s.a, s.b, contour.value()); !line) return line.error();
    if (auto arc = drawing().addArc(doc, {o.x + w - r, o.y + h - r}, r, 0.0, 1.5707963267948966, contour.value()); !arc)
        return arc.error();

    const Point2 c{o.x + w / 2.0, o.y + h / 2.0};
    if (auto hole = drawing().addCircle(doc, c, 15.0, contour.value()); !hole) return hole.error();
    if (auto ax = drawing().addLine(doc, {c.x - 20.0, c.y}, {c.x + 20.0, c.y}, axes.value()); !ax) return ax.error();
    if (auto ay = drawing().addLine(doc, {c.x, c.y - 20.0}, {c.x, c.y + 20.0}, axes.value()); !ay) return ay.error();

    if (auto d = drawing().addLinearDimension(doc, {o.x, o.y}, {o.x + w, o.y}, -12.0, dimensions.value()); !d)
        return d.error();
    if (auto d = drawing().addLinearDimension(doc, {o.x, o.y}, {o.x, o.y + h}, 12.0, dimensions.value()); !d)
        return d.error();
    if (auto d = drawing().addRadialDimension(doc, c, 15.0, 2.2, true, dimensions.value()); !d) return d.error();
    if (auto d = drawing().addRadialDimension(doc, {o.x + w - r, o.y + h - r}, r, 0.7853981633974483, false,
                                              dimensions.value());
        !d)
        return d.error();
    if (auto t = drawing().addText(doc, {o.x, o.y + h + 12.0}, "Пластина. Пример чертежа CosmoCad", 5.0,
                                  dimensions.value());
        !t)
        return t.error();
    return &doc;
}

Result<document::Document*> AppContext::createDemoSchematic() {
    auto eskd = standards_->profile("eskd");
    if (!eskd) return eskd.error();
    auto created = documents_->createDocument(document::DocumentKind::Drawing, "Фильтр. Схема");
    if (!created) return created;
    document::Document& doc = *created.value();
    drawing::SheetSetup setup;
    setup.profileId = "eskd";
    setup.profileVersion = eskd.value().version;
    setup.format = "A4";
    if (auto sheet = drawing().setupSheet(doc, setup); !sheet) return sheet.error();
    for (const auto& [field, value] : {std::pair{"designation", "КСМ.004.001 Э3"}, std::pair{"title", "Фильтр"},
                                       std::pair{"developer", "Иванов"}, std::pair{"organization", "CosmoCad"},
                                       std::pair{"scale", "-"}, std::pair{"sheetCount", "1"}})
        if (auto set = drawing().setTitleBlockField(doc, field, value); !set) return set.error();

    auto& s = schematic();
    // Предохранитель и резистор на входе; конденсатор, защитный диод и нагрузка — между выходом и общим проводом.
    if (auto e = s.addElement(doc, "fuse", {55, 200}, 0, "0,5 А"); !e) return e.error();
    if (auto e = s.addElement(doc, "resistor", {80, 200}, 0, "10 кОм"); !e) return e.error();
    if (auto e = s.addElement(doc, "capacitor", {110, 180}, 90, "0,1 мкФ"); !e) return e.error();
    if (auto e = s.addElement(doc, "diode", {140, 180}, 90, ""); !e) return e.error();
    if (auto e = s.addElement(doc, "resistor", {165, 180}, 90, "10 кОм"); !e) return e.error();
    const std::vector<std::vector<Point2>> wires = {
        {{35, 200}, {45, 200}},    // вход → FU1
        {{65, 200}, {70, 200}},    // FU1 → R1
        {{90, 200}, {190, 200}},   // R1 → выход
        {{110, 190}, {110, 200}},  // C1 вверх, к линии выхода
        {{140, 190}, {140, 200}},  // VD1 вверх (катод)
        {{165, 190}, {165, 200}},  // R2 вверх
        {{110, 170}, {110, 160}},  // C1 вниз, к общему проводу
        {{140, 170}, {140, 160}},  // VD1 вниз (анод)
        {{165, 170}, {165, 160}},  // R2 вниз
        {{35, 160}, {190, 160}},   // общий провод
    };
    for (const auto& points : wires)
        if (auto w = s.addWire(doc, points); !w) return w.error();
    for (const auto& [at, name] : {std::pair{Point2{35, 200}, "Вход"}, std::pair{Point2{190, 200}, "Выход"},
                                   std::pair{Point2{35, 160}, "Общий"}, std::pair{Point2{190, 160}, "Общий"}})
        if (auto l = s.addLabel(doc, at, name); !l) return l.error();
    if (auto n = s.renumber(doc); !n) return n.error();
    if (auto table = placeElementListTable(doc); !table) return table.error();
    return &doc;
}

std::vector<std::string> AppContext::disabledPlugins() const {
    std::vector<std::string> disabled;
    if (userDirectory_.empty()) return disabled;
    std::ifstream in(userDirectory_ / "plugins-disabled.txt");
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) disabled.push_back(line);
    }
    return disabled;
}

Result<void> AppContext::setPluginDisabled(const std::string& pluginId, bool disabled) {
    if (userDirectory_.empty()) return core::makeError(core::ErrorCode::InvalidState, "папка пользователя не задана");
    if (pluginId.empty() || pluginId.find('\n') != std::string::npos) return core::makeError(core::ErrorCode::InvalidArgument, "неверное имя дополнения");
    std::vector<std::string> list = disabledPlugins();
    const auto it = std::find(list.begin(), list.end(), pluginId);
    if (disabled && it == list.end()) list.push_back(pluginId);
    if (!disabled && it != list.end()) list.erase(it);
    std::error_code problem;
    std::filesystem::create_directories(userDirectory_, problem);
    std::ofstream out(userDirectory_ / "plugins-disabled.txt");
    if (!out) return core::makeError(core::ErrorCode::IoError, "не удалось записать список выключенных дополнений");
    for (const auto& id : list) out << id << '\n';
    return {};
}

int AppContext::loadPlugins(const std::filesystem::path& executableDirectory) {
    const std::vector<std::string> disabled = disabledPlugins();
    int active = 0;
    // Папка сборки и папка установки (bin/../share/cosmocad/plugins).
    for (const auto& folder : {executableDirectory / "plugins", executableDirectory.parent_path() / "plugins",
                               executableDirectory.parent_path() / "share" / "cosmocad" / "plugins"})
        active += plugins_->loadFrom(folder, disabled);
    if (!userDirectory_.empty()) active += plugins_->loadFrom(userDirectory_ / "plugins", disabled);
    if (const char* extra = std::getenv("COSMOCAD_PLUGIN_DIR"); extra && *extra) active += plugins_->loadFrom(extra, disabled);
    return active;
}

Result<void> AppContext::setPartMaterial(document::Document& part, const std::string& materialTitle) {
    const libraries::Material* material = materials_->byTitle(materialTitle);
    if (!material) return core::makeError(core::ErrorCode::NotFound, "в библиотеке нет материала «" + materialTitle + "»");
    std::string designation;
    if (const auto properties = part.objectsOf<modeling::PartProperties>(); !properties.empty()) designation = properties.front()->designation;
    return modeling().setPartProperties(part, designation, material->title(), material->density);
}

Result<document::Document*> AppContext::standardPart(const libraries::FastenerRequest& request) {
    auto info = fasteners().describe(request);
    if (!info) return info.error();
    for (const auto& document : documents().project().documents())
        if (document->kind() == document::DocumentKind::Part && document->name == info.value().designation) return document.get();
    auto created = documents().createDocument(document::DocumentKind::Part, info.value().designation);
    if (!created) return created.error();
    // Не получилось построить — недоделанная деталь в проекте не остаётся.
    const auto discard = [&](const core::Error& error) {
        const core::Uuid id = created.value()->id();
        modeling().forget(id);
        (void)documents().removeDocument(id);
        return error;
    };
    if (auto built = fasteners().build(modeling(), *created.value(), request); !built) return discard(built.error());
    auto report = modeling().recompute(*created.value());
    if (!report) return discard(report.error());
    for (const auto& status : report.value().features)
        if (status.state == modeling::FeatureState::Failed)
            return discard(core::makeError(core::ErrorCode::Internal, "изделие из библиотеки не построилось: " + status.diagnostic));
    return created.value();
}

Result<document::Document*> AppContext::anglePart(double b, double t, double length) {
    auto name = profiles().angleDesignation(b, t, length);
    if (!name) return name.error();
    for (const auto& document : documents().project().documents())
        if (document->kind() == document::DocumentKind::Part && document->name == name.value()) return document.get();
    auto created = documents().createDocument(document::DocumentKind::Part, name.value());
    if (!created) return created.error();
    const auto discard = [&](const core::Error& error) {
        const core::Uuid id = created.value()->id();
        modeling().forget(id);
        (void)documents().removeDocument(id);
        return error;
    };
    if (auto built = profiles().buildAngle(modeling(), *created.value(), b, t, length); !built) return discard(built.error());
    auto report = modeling().recompute(*created.value());
    if (!report) return discard(report.error());
    for (const auto& status : report.value().features)
        if (status.state == modeling::FeatureState::Failed)
            return discard(core::makeError(core::ErrorCode::Internal, "уголок не построился: " + status.diagnostic));
    return created.value();
}

Result<document::Document*> AppContext::profilePart(const std::string& kind, const std::string& name, double length) {
    auto title = profiles().designation(kind, name, length);
    if (!title) return title.error();
    for (const auto& document : documents().project().documents())
        if (document->kind() == document::DocumentKind::Part && document->name == title.value()) return document.get();
    auto created = documents().createDocument(document::DocumentKind::Part, title.value());
    if (!created) return created.error();
    const auto discard = [&](const core::Error& error) {
        const core::Uuid id = created.value()->id();
        modeling().forget(id);
        (void)documents().removeDocument(id);
        return error;
    };
    if (auto built = profiles().build(modeling(), *created.value(), kind, name, length); !built) return discard(built.error());
    auto report = modeling().recompute(*created.value());
    if (!report) return discard(report.error());
    for (const auto& status : report.value().features)
        if (status.state == modeling::FeatureState::Failed)
            return discard(core::makeError(core::ErrorCode::Internal, "профиль не построился: " + status.diagnostic));
    return created.value();
}

Result<document::Document*> AppContext::pipePart(const std::string& material, double diameter, double wall, double length) {
    const std::string name = pipes().designation(material, diameter, wall, length);
    for (const auto& document : documents().project().documents())
        if (document->kind() == document::DocumentKind::Part && document->name == name) return document.get();
    auto created = documents().createDocument(document::DocumentKind::Part, name);
    if (!created) return created.error();
    const auto discard = [&](const core::Error& error) {
        const core::Uuid id = created.value()->id();
        modeling().forget(id);
        (void)documents().removeDocument(id);
        return error;
    };
    if (auto built = pipes().build(modeling(), *created.value(), material, diameter, wall, length); !built) return discard(built.error());
    auto report = modeling().recompute(*created.value());
    if (!report) return discard(report.error());
    for (const auto& status : report.value().features)
        if (status.state == modeling::FeatureState::Failed)
            return discard(core::makeError(core::ErrorCode::Internal, "труба не построилась: " + status.diagnostic));
    return created.value();
}

namespace {
const char* const kRequirementName = "Технические требования";
const char* const kRequirementContinuation = "Технические требования: продолжение";
}  // namespace

std::vector<std::string> AppContext::technicalRequirements(const document::Document& document) const {
    // Строки стоят сверху вниз; продолжение пункта приклеивается к его началу.
    std::vector<const drawing::TextEntity*> lines;
    for (const auto* text : document.objectsOf<drawing::TextEntity>())
        if (text->name == kRequirementName || text->name == kRequirementContinuation) lines.push_back(text);
    std::stable_sort(lines.begin(), lines.end(), [](const auto* a, const auto* b) { return a->position.y > b->position.y; });
    std::vector<std::string> items;
    for (const auto* line : lines) {
        if (line->name == kRequirementName) {
            const auto space = line->text.find(' ');
            items.push_back(space == std::string::npos ? line->text : line->text.substr(space + 1));
        } else if (!items.empty()) {
            items.back() += " " + line->text;
        }
    }
    return items;
}

Result<void> AppContext::setTechnicalRequirements(document::Document& document, const std::vector<std::string>& items) {
    const auto sheets = document.objectsOf<drawing::Sheet>();
    auto profile = profileFor(document);
    if (sheets.empty() || !profile) return core::makeError(core::ErrorCode::InvalidState, "на листе нет рамки и основной надписи");
    const auto geometry = standards::sheetGeometry(*sheets.front(), *profile);
    if (!geometry) return core::makeError(core::ErrorCode::InvalidState, "не удалось определить место основной надписи");
    for (const auto& item : items)
        if (item.find_first_not_of(" \t") == std::string::npos)
            return core::makeError(core::ErrorCode::InvalidArgument, "пункт технических требований пуст");

    // Колонка — над основной надписью, не шире 185 мм. Строки переносятся по словам.
    const double height = 3.5, pitch = height * 1.7, charWidth = height * 0.62;
    // Текст отступает от линии рамки на 2 мм, чтобы буквы её не касались.
    const double left = geometry->titleBlock.min.x + 2.0, columnWidth = std::min(185.0, geometry->titleBlock.width()) - 2.0;
    const auto limit = static_cast<std::size_t>(columnWidth / charWidth);
    // Каждый пункт начинается с абзацного отступа в пять знаков шрифта
    // (ГОСТ Р 2.316-2023, п. 6.6; ГОСТ Р 2.105-2019, п. 5.1.4); остальные строки — от края колонки.
    const std::size_t indentChars = 5;
    struct Line {
        std::string text;
        bool first;
    };
    std::vector<Line> lines;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::istringstream words(std::to_string(i + 1) + " " + items[i]);
        std::string word, current;
        bool first = true;
        while (words >> word) {
            if (!current.empty() &&
                drawing::characterCount(current) + 1 + drawing::characterCount(word) > limit - (first ? indentChars : 0)) {
                lines.push_back({current, first});
                first = false;
                current.clear();
            }
            current += (current.empty() ? "" : " ") + word;
        }
        if (!current.empty()) lines.push_back({current, first});
    }
    const double bottom = geometry->titleBlock.max.y + 5.0;
    const double top = bottom + pitch * static_cast<double>(lines.size());
    if (top > geometry->frame.max.y)
        return core::makeError(core::ErrorCode::InvalidState, "технические требования не помещаются над основной надписью");

    std::vector<core::Uuid> old;
    for (const auto* text : document.objectsOf<drawing::TextEntity>())
        if (text->name == kRequirementName || text->name == kRequirementContinuation) old.push_back(text->id());
    if (old.empty() && lines.empty()) return {};
    document::Transaction tx(document, lines.empty() ? "Технические требования убраны" : "Технические требования");
    for (const auto& id : old)
        if (auto ok = tx.remove(id); !ok) return ok;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto text = std::make_unique<drawing::TextEntity>();
        text->name = lines[i].first ? kRequirementName : kRequirementContinuation;
        text->text = lines[i].text;
        text->height = height;
        // Первая строка — наверху; продолжение пункта начинается под его текстом, а не под номером.
        text->position = {left + (lines[i].first ? static_cast<double>(indentChars) * charWidth : 0.0), top - pitch * static_cast<double>(i + 1) + (pitch - height) / 2.0};
        if (auto id = tx.add(std::move(text)); !id) return id.error();
    }
    tx.commit();
    return {};
}

Result<void> AppContext::setSheetRoughness(document::Document& document, const std::string& parameter,
                                           drawing::RoughnessSign::Kind kind, bool others) {
    static const char* const kName = "Шероховатость: общий знак";
    const auto sheets = document.objectsOf<drawing::Sheet>();
    auto profile = profileFor(document);
    if (sheets.empty() || !profile) return core::makeError(core::ErrorCode::InvalidState, "на листе нет рамки");
    const auto geometry = standards::sheetGeometry(*sheets.front(), *profile);
    if (!geometry) return core::makeError(core::ErrorCode::InvalidState, "не удалось определить рамку листа");
    std::vector<core::Uuid> old;
    for (const auto* sign : document.objectsOf<drawing::RoughnessSign>())
        if (sign->name == kName) old.push_back(sign->id());
    const bool remove = parameter.empty() && !others && kind == drawing::RoughnessSign::Kind::Any;
    if (remove && old.empty()) return {};

    if (auto ok = drawing::checkRoughnessParameter(parameter); !ok) return ok;
    document::Transaction tx(document, remove ? "Общий знак шероховатости убран" : "Общий знак шероховатости");
    for (const auto& id : old)
        if (auto ok = tx.remove(id); !ok) return ok;
    if (!remove) {
        // Знаки на изображении — высотой 3,5 мм; общий знак — следующего размера шрифта (5 мм, в 1,4 раза крупнее).
        auto sign = std::make_unique<drawing::RoughnessSign>();
        sign->name = kName;
        sign->kind = kind;
        sign->parameter = parameter;
        sign->textHeight = 5.0;
        sign->others = others;
        sign->othersHeight = 3.5;
        sign->lineTypeId = "solid_thin";
        sign->lineWidth = sheets.front()->mainLineWidth * 0.5 * 1.5;  // линии тоже в 1,5 раза толще
        const auto box = sign->bounds();  // при вершине в начале координат
        // Знак стоит в правом верхнем углу, в 7,5 мм от линий рамки.
        sign->position = {geometry->frame.max.x - 7.5 - box.max.x, geometry->frame.max.y - 7.5 - box.max.y};
        if (auto id = tx.add(std::move(sign)); !id) return id.error();
    }
    tx.commit();
    return {};
}

Result<int> AppContext::placeElementListTable(document::Document& document) {
    const auto sheets = document.objectsOf<drawing::Sheet>();
    auto profile = profileFor(document);
    if (sheets.empty() || !profile) return core::makeError(core::ErrorCode::InvalidState, "на листе нет рамки и основной надписи");
    const auto geometry = standards::sheetGeometry(*sheets.front(), *profile);
    if (!geometry) return core::makeError(core::ErrorCode::InvalidState, "не удалось определить место основной надписи");
    const auto rows = schematic().elementList(document);
    if (rows.empty()) return core::makeError(core::ErrorCode::InvalidState, "на листе нет элементов схемы");

    const double head = 15.0, rowHeight = 8.0, gap = 12.0;
    const double widths[4] = {20.0, 110.0, 10.0, 45.0};
    const double right = geometry->titleBlock.max.x, left = right - 185.0;
    const double bottom = geometry->titleBlock.max.y + gap;
    const double top = bottom + head + rowHeight * static_cast<double>(rows.size());
    if (top > geometry->frame.max.y)
        return core::makeError(core::ErrorCode::InvalidState,
                               "перечень не помещается над основной надписью: выпустите его отдельным документом");

    document::Transaction tx(document, "Перечень элементов на листе");
    // Прежняя таблица убирается: её части помечены именем.
    const std::string mark = "Перечень элементов";
    for (const auto* entity : document.objectsOf<drawing::Entity>())
        if (entity->name == mark)
            if (auto ok = tx.remove(entity->id()); !ok) return ok.error();
    const auto line = [&](Point2 a, Point2 b, bool main) -> Result<void> {
        auto entity = std::make_unique<drawing::LineEntity>();
        entity->name = mark;
        entity->start = a;
        entity->end = b;
        if (!main) entity->lineTypeId = "solid_thin";
        auto id = tx.add(std::move(entity));
        if (!id) return id.error();
        return {};
    };
    const auto text = [&](double x, double y, const std::string& value, double height = 3.5) -> Result<void> {
        if (value.empty()) return {};
        auto entity = std::make_unique<drawing::TextEntity>();
        entity->name = mark;
        entity->position = {x, y};
        entity->text = value;
        entity->height = height;
        entity->lineTypeId = "solid_thin";
        auto id = tx.add(std::move(entity));
        if (!id) return id.error();
        return {};
    };
    // Рамка и графы — основной линией, строки — тонкой.
    if (auto ok = line({left, top}, {right, top}, true); !ok) return ok.error();
    if (auto ok = line({left, bottom}, {right, bottom}, true); !ok) return ok.error();
    if (auto ok = line({left, top - head}, {right, top - head}, true); !ok) return ok.error();
    double x = left;
    for (int column = 0; column <= 4; ++column) {
        if (auto ok = line({x, bottom}, {x, top}, true); !ok) return ok.error();
        if (column < 4) x += widths[column];
    }
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const double y = top - head - rowHeight * static_cast<double>(i);
        if (auto ok = line({left, y}, {right, y}, false); !ok) return ok.error();
    }
    const double c0 = left + 1.5, c1 = left + 20.0 + 2.0, c2 = left + 130.0 + 2.5, c3 = left + 140.0 + 2.0;
    if (auto ok = text(c0, top - 6.5, "Поз."); !ok) return ok.error();
    if (auto ok = text(c0, top - 12.5, "обозн."); !ok) return ok.error();
    if (auto ok = text(c1 + 35.0, top - 9.5, "Наименование"); !ok) return ok.error();
    if (auto ok = text(c2 - 1.3, top - 9.0, "Кол.", 2.5); !ok) return ok.error();  // графа узкая: шрифт 2,5
    if (auto ok = text(c3 + 6.0, top - 9.5, "Примечание"); !ok) return ok.error();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const double y = top - head - rowHeight * static_cast<double>(i + 1) + 2.2;
        if (auto ok = text(c0, y, rows[i].designators); !ok) return ok.error();
        if (auto ok = text(c1, y, rows[i].name); !ok) return ok.error();
        if (auto ok = text(c2, y, std::to_string(rows[i].count)); !ok) return ok.error();
    }
    tx.commit();
    return static_cast<int>(rows.size());
}

}  // namespace cosmocad::app
