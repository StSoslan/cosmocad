#include "Batch.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <map>

#include "cosmocad/modeling/Expression.h"

namespace cosmocad::app {

using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Uuid;
using document::Document;
using document::DocumentKind;
using nlohmann::json;

namespace {

struct State {
    AppContext& context;
    std::map<std::string, Uuid> sketches;    // имя из "as" → эскиз
    std::map<std::string, Uuid> components;  // имя из "as" → компонент
};

Result<Document*> documentOf(State& s, const json& c, const char* field, DocumentKind kind) {
    if (!c.contains(field) || !c.at(field).is_string())
        return makeError(ErrorCode::InvalidArgument, std::string("не указано поле «") + field + "» — имя документа");
    const std::string name = c.at(field).get<std::string>();
    for (const auto& document : s.context.documents().project().documents())
        if (document->name == name) {
            if (document->kind() != kind)
                return makeError(ErrorCode::InvalidArgument, "документ «" + name + "» другого вида, чем нужно команде");
            return document.get();
        }
    return makeError(ErrorCode::NotFound, "в проекте нет документа «" + name + "»");
}

// Число или формула: и то и другое передаётся дальше текстом формулы.
Result<std::string> expression(const json& c, const char* field, const char* fallback = nullptr) {
    if (!c.contains(field)) {
        if (fallback) return std::string(fallback);
        return makeError(ErrorCode::InvalidArgument, std::string("не указано поле «") + field + "»");
    }
    const json& v = c.at(field);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return modeling::formatNumber(v.get<double>());
    return makeError(ErrorCode::InvalidArgument, std::string("поле «") + field + "» должно быть числом или формулой");
}

Result<Uuid> named(const std::map<std::string, Uuid>& table, const json& c, const char* field, const char* what) {
    if (!c.contains(field) || !c.at(field).is_string())
        return makeError(ErrorCode::InvalidArgument, std::string("не указано поле «") + field + "»");
    const auto it = table.find(c.at(field).get<std::string>());
    if (it == table.end())
        return makeError(ErrorCode::NotFound, std::string(what) + " «" + c.at(field).get<std::string>() + "» не найден: имя задаётся полем \"as\"");
    return it->second;
}

Result<json> execute(State& s, const json& c) {
    auto& context = s.context;
    auto& model = context.modeling();
    const std::string action = c.value("do", std::string());
    json reply = {{"do", action}};

    if (action == "new_part" || action == "new_assembly") {
        const std::string name = c.value("name", std::string());
        if (name.empty()) return makeError(ErrorCode::InvalidArgument, "не указано имя нового документа");
        for (const auto& document : context.documents().project().documents())
            if (document->name == name) return makeError(ErrorCode::InvalidState, "документ «" + name + "» уже есть в проекте");
        auto created = context.documents().createDocument(action == "new_part" ? DocumentKind::Part : DocumentKind::Assembly, name);
        if (!created) return created.error();
        reply["name"] = name;
    } else if (action == "parameter") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        const std::string name = c.value("name", std::string());
        auto value = expression(c, "value");
        if (!value) return value.error();
        const auto values = model.parameterValues(*part.value());
        if (values && values.value().count(name)) {
            if (auto ok = model.setParameter(*part.value(), name, value.value()); !ok) return ok.error();
            if (auto synced = context.syncSketches(*part.value()); !synced) return synced.error();
        } else if (auto ok = model.addParameter(*part.value(), name, value.value()); !ok) {
            return ok.error();
        }
    } else if (action == "sketch") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        modeling::SketchParams params;
        params.name = c.value("name", std::string());
        params.plane = c.value("plane", std::string("XY"));
        params.faceName = c.value("face", std::string());
        auto offset = expression(c, "offset", "0");
        if (!offset) return offset.error();
        params.offset = offset.value();
        if (!c.contains("shapes") || !c.at("shapes").is_array() || c.at("shapes").empty())
            return makeError(ErrorCode::InvalidArgument, "у эскиза нет фигур: поле \"shapes\"");
        for (const auto& item : c.at("shapes")) {
            modeling::SketchShape shape;
            const std::string kind = item.value("kind", std::string());
            if (kind == "rectangle")
                shape.kind = modeling::SketchShape::Kind::Rectangle;
            else if (kind == "circle")
                shape.kind = modeling::SketchShape::Kind::Circle;
            else if (kind == "polygon")
                shape.kind = modeling::SketchShape::Kind::Polygon;
            else
                return makeError(ErrorCode::InvalidArgument, "неизвестный вид фигуры «" + kind + "»: rectangle, circle или polygon");
            shape.hole = item.value("hole", false);
            if (!item.contains("values") || !item.at("values").is_array()) return makeError(ErrorCode::InvalidArgument, "у фигуры нет размеров: поле \"values\"");
            for (const auto& v : item.at("values")) {
                if (v.is_string())
                    shape.values.push_back(v.get<std::string>());
                else if (v.is_number())
                    shape.values.push_back(modeling::formatNumber(v.get<double>()));
                else
                    return makeError(ErrorCode::InvalidArgument, "размер фигуры должен быть числом или формулой");
            }
            params.shapes.push_back(std::move(shape));
        }
        auto id = model.addSketch(*part.value(), params);
        if (!id) return id.error();
        if (c.contains("as")) s.sketches[c.at("as").get<std::string>()] = id.value();
    } else if (action == "extrude" || action == "revolve" || action == "sweep") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        auto sketch = named(s.sketches, c, "sketch", "эскиз");
        if (!sketch) return sketch.error();
        Result<Uuid> feature = Uuid();
        if (action == "extrude") {
            auto distance = expression(c, "distance");
            if (!distance) return distance.error();
            feature = model.addExtrude(*part.value(), {sketch.value(), distance.value(), c.value("cut", false), c.value("reversed", false)});
        } else if (action == "revolve") {
            auto angle = expression(c, "angle", "360");
            if (!angle) return angle.error();
            feature = model.addRevolve(*part.value(), {sketch.value(), c.value("axis", std::string("Y")), angle.value(), c.value("cut", false)});
        } else {
            modeling::SweepParams params;
            params.sketchId = sketch.value();
            params.cut = c.value("cut", false);
            auto radius = expression(c, "radius", "0");
            if (!radius) return radius.error();
            params.bendRadius = radius.value();
            if (!c.contains("path") || !c.at("path").is_array()) return makeError(ErrorCode::InvalidArgument, "у протяжки нет траектории: поле \"path\"");
            for (const auto& point : c.at("path")) {
                if (!point.is_array() || point.size() != 3) return makeError(ErrorCode::InvalidArgument, "точка траектории — три числа или формулы: [x, y, z]");
                std::array<std::string, 3> p;
                for (std::size_t i = 0; i < 3; ++i) p[i] = point[i].is_string() ? point[i].get<std::string>() : modeling::formatNumber(point[i].get<double>());
                params.path.push_back(p);
            }
            feature = model.addSweep(*part.value(), params);
        }
        if (!feature) return feature.error();
    } else if (action == "fillet" || action == "chamfer") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        auto size = expression(c, "size");
        if (!size) return size.error();
        modeling::BlendParams params;
        params.size = size.value();
        params.chamfer = action == "chamfer";
        if (!c.contains("edges") || !c.at("edges").is_array()) return makeError(ErrorCode::InvalidArgument, "не указаны рёбра: поле \"edges\" — пары имён граней");
        for (const auto& edge : c.at("edges")) {
            if (!edge.is_array() || edge.size() != 2) return makeError(ErrorCode::InvalidArgument, "ребро задаётся парой имён граней");
            params.edges.push_back({edge[0].get<std::string>(), edge[1].get<std::string>()});
        }
        if (auto id = model.addBlend(*part.value(), params); !id) return id.error();
    } else if (action == "mirror") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        auto offset = expression(c, "offset", "0");
        if (!offset) return offset.error();
        if (auto id = model.addMirror(*part.value(), {c.value("plane", std::string("YZ")), offset.value()}); !id) return id.error();
    } else if (action == "material") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        if (auto ok = context.setPartMaterial(*part.value(), c.value("title", std::string())); !ok) return ok.error();
    } else if (action == "designation") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        std::string material = "Сталь";
        double density = 7.85;
        if (const auto properties = part.value()->objectsOf<modeling::PartProperties>(); !properties.empty()) {
            material = properties.front()->material;
            density = properties.front()->density;
        }
        if (auto ok = model.setPartProperties(*part.value(), c.value("value", std::string()), material, density); !ok) return ok.error();
    } else if (action == "save_configuration" || action == "apply_configuration") {
        auto part = documentOf(s, c, "part", DocumentKind::Part);
        if (!part) return part.error();
        const std::string name = c.value("name", std::string());
        auto ok = action == "save_configuration" ? model.saveConfiguration(*part.value(), name, c.value("designation", std::string()))
                                                 : model.applyConfiguration(*part.value(), name);
        if (!ok) return ok.error();
    } else if (action == "fastener") {
        libraries::FastenerRequest request;
        const std::string kind = c.value("kind", std::string("bolt"));
        if (kind == "bolt")
            request.kind = libraries::FastenerKind::Bolt;
        else if (kind == "nut")
            request.kind = libraries::FastenerKind::Nut;
        else if (kind == "washer")
            request.kind = libraries::FastenerKind::Washer;
        else
            return makeError(ErrorCode::InvalidArgument, "неизвестный вид крепежа «" + kind + "»: bolt, nut или washer");
        request.d = c.value("d", 0.0);
        request.length = c.value("length", 0.0);
        auto part = context.standardPart(request);
        if (!part) return part.error();
        reply["name"] = part.value()->name;
    } else if (action == "channel" || action == "beam") {
        auto part = context.profilePart(action, c.value("name", std::string()), c.value("length", 0.0));
        if (!part) return part.error();
        reply["name"] = part.value()->name;
    } else if (action == "angle") {
        auto part = context.anglePart(c.value("b", 0.0), c.value("t", 0.0), c.value("length", 0.0));
        if (!part) return part.error();
        reply["name"] = part.value()->name;
    } else if (action == "pipe" || action == "pipe_check") {
        const auto& pipes = context.pipes();
        const std::string material = c.value("material", std::string("PP-R"));
        const double diameter = c.value("d", 0.0);
        double wall = c.value("wall", 0.0);
        if (wall <= 0.0) {
            auto found = pipes.wall(diameter, c.value("sdr", 0.0));
            if (!found) return found.error();
            wall = found.value();
        }
        reply["wall"] = wall;
        if (action == "pipe") {
            auto part = context.pipePart(material, diameter, wall, c.value("length", 0.0));
            if (!part) return part.error();
            reply["name"] = part.value()->name;
        } else {
            auto report = pipes.check(material, c.value("class", std::string()), diameter, wall, c.value("pressure", 0.0));
            if (!report) return report.error();
            const auto& r = report.value();
            reply["fits"] = r.ok;
            reply["stress_mpa"] = r.stress;
            reply["design_stress_mpa"] = r.designStress;
            reply["allowed_pressure_mpa"] = r.allowedPressure;
            reply["minimum_wall_mm"] = r.minimumWall;
            reply["life_years"] = r.lifeYears;
            reply["sdr"] = r.sdr;
            reply["report"] = r.lines;
        }
    } else if (action == "component") {
        auto target = documentOf(s, c, "assembly", DocumentKind::Assembly);
        if (!target) return target.error();
        const std::string source = c.value("source", std::string());
        Document* found = nullptr;
        for (const auto& document : context.documents().project().documents())
            if (document->name == source) found = document.get();
        if (!found) return makeError(ErrorCode::NotFound, "в проекте нет документа «" + source + "»");
        std::array<double, 3> at{0, 0, 0};
        if (c.contains("at")) {
            if (!c.at("at").is_array() || c.at("at").size() != 3) return makeError(ErrorCode::InvalidArgument, "место компонента — три числа: [x, y, z]");
            at = c.at("at").get<std::array<double, 3>>();
        }
        auto id = context.assembly().addComponent(*target.value(), found->id(), geometry::Placement::translation(at[0], at[1], at[2]));
        if (!id) return id.error();
        if (c.contains("as")) s.components[c.at("as").get<std::string>()] = id.value();
    } else if (action == "mate") {
        auto target = documentOf(s, c, "assembly", DocumentKind::Assembly);
        if (!target) return target.error();
        static const std::map<std::string, assembly::MateKind> kinds = {
            {"coincident", assembly::MateKind::Coincident}, {"concentric", assembly::MateKind::Concentric},
            {"parallel", assembly::MateKind::Parallel},     {"distance", assembly::MateKind::Distance},
            {"angle", assembly::MateKind::Angle},           {"tangent", assembly::MateKind::Tangent}};
        const auto kind = kinds.find(c.value("kind", std::string()));
        if (kind == kinds.end()) return makeError(ErrorCode::InvalidArgument, "неизвестный вид сопряжения «" + c.value("kind", std::string()) + "»");
        assembly::MateParams params;
        params.kind = kind->second;
        params.value = c.value("value", 0.0);
        params.flip = c.value("flip", false);
        for (int i = 0; i < 2; ++i) {
            const char* field = i == 0 ? "first" : "second";
            if (!c.contains(field) || !c.at(field).is_array() || c.at(field).size() != 2)
                return makeError(ErrorCode::InvalidArgument, std::string("поле «") + field + "» — пара [имя компонента, имя грани]");
            const auto it = s.components.find(c.at(field)[0].get<std::string>());
            if (it == s.components.end()) return makeError(ErrorCode::NotFound, "компонент «" + c.at(field)[0].get<std::string>() + "» не найден: имя задаётся полем \"as\"");
            (i == 0 ? params.first : params.second) = assembly::GeometryRef{it->second, c.at(field)[1].get<std::string>()};
        }
        if (auto id = context.assembly().addMate(*target.value(), params); !id) return id.error();
        auto solved = context.assembly().solve(*target.value());
        if (!solved) return solved.error();
        if (!solved.value().solved) return makeError(ErrorCode::InvalidState, solved.value().conflicts.front().second);
    } else if (action == "info") {
        const std::string name = c.value("document", std::string());
        Document* found = nullptr;
        for (const auto& document : context.documents().project().documents())
            if (document->name == name) found = document.get();
        if (!found) return makeError(ErrorCode::NotFound, "в проекте нет документа «" + name + "»");
        if (found->kind() == DocumentKind::Part) {
            auto report = model.recompute(*found);
            if (!report) return report.error();
            json failures = json::array();
            const auto features = model.features(*found);
            for (std::size_t i = 0; i < report.value().features.size(); ++i)
                if (report.value().features[i].state == modeling::FeatureState::Failed)
                    failures.push_back(features[i]->name + ": " + report.value().features[i].diagnostic);
            reply["failures"] = failures;
            if (auto body = model.body(*found)) {
                auto mass = model.mass(*found);
                if (!mass) return mass.error();
                reply["volume_mm3"] = mass.value().geometry.volume;
                reply["mass_g"] = mass.value().massGrams;
                reply["faces"] = context.kernel().faceNames(body);
            }
            json parameters = json::object();
            if (auto values = model.parameterValues(*found))
                for (const auto& [key, value] : values.value()) parameters[key] = value;
            reply["parameters"] = parameters;
        } else if (found->kind() == DocumentKind::Assembly) {
            auto bom = context.assembly().billOfMaterials(*found);
            if (!bom) return bom.error();
            json lines = json::array();
            for (const auto& line : bom.value())
                lines.push_back({{"designation", line.designation}, {"name", line.name}, {"quantity", line.quantity}, {"mass_g", line.massGrams}});
            reply["bill_of_materials"] = lines;
            auto hits = context.assembly().checkInterference(*found);
            if (!hits) return hits.error();
            reply["interferences"] = hits.value().size();
        } else {
            return makeError(ErrorCode::InvalidArgument, "сведения выдаются о детали или сборке");
        }
    } else if (action == "export_step" || action == "export_stl") {
        const std::string name = c.value("document", std::string()), path = c.value("path", std::string());
        Document* found = nullptr;
        for (const auto& document : context.documents().project().documents())
            if (document->name == name) found = document.get();
        if (!found) return makeError(ErrorCode::NotFound, "в проекте нет документа «" + name + "»");
        if (path.empty()) return makeError(ErrorCode::InvalidArgument, "не указан файл: поле \"path\"");
        auto bodies = context.bodiesOf(*found);
        if (!bodies) return bodies.error();
        if (action == "export_step") {
            if (auto ok = context.kernel().exportStep(bodies.value(), path); !ok) return ok.error();
        } else {
            auto whole = context.kernel().combine(bodies.value());
            if (!whole) return whole.error();
            if (auto ok = context.kernel().exportStl(whole.value(), path, 0.05); !ok) return ok.error();
        }
        reply["path"] = path;
    } else if (action == "plugin") {
        // Команда дополнения: её имя — в поле "command", остальные поля передаются дополнению.
        json fields = c.value("args", json::object());
        auto result = context.plugins().run(c.value("command", std::string()), fields);
        if (!result) return result.error();
        reply["result"] = result.value();
    } else if (action == "documents") {
        json list = json::array();
        for (const auto& document : context.documents().project().documents()) {
            const char* kind = document->kind() == DocumentKind::Part ? "part" : document->kind() == DocumentKind::Assembly ? "assembly" : "drawing";
            list.push_back({{"name", document->name}, {"kind", kind}});
        }
        reply["documents"] = list;
    } else {
        return makeError(ErrorCode::InvalidArgument, "неизвестная команда «" + action + "»");
    }
    reply["ok"] = true;
    return reply;
}

}  // namespace

Result<json> runBatch(AppContext& context, const json& commands) {
    if (!commands.is_array()) return makeError(ErrorCode::InvalidArgument, "список команд должен быть массивом");
    State state{context, {}, {}};
    json replies = json::array();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        Result<json> reply = makeError(ErrorCode::Internal, "команда не выполнена");
        try {
            if (!commands[i].is_object()) return makeError(ErrorCode::InvalidArgument, "команда " + std::to_string(i + 1) + ": должна быть объектом с полем \"do\"");
            reply = execute(state, commands[i]);
        } catch (const json::exception& e) {
            reply = makeError(ErrorCode::InvalidArgument, std::string("неверный вид поля: ") + e.what());
        }
        if (!reply)
            return makeError(reply.error().code, "команда " + std::to_string(i + 1) + " («" + commands[i].value("do", std::string("?")) + "»): " + reply.error().message);
        replies.push_back(reply.take());
    }
    return replies;
}

}  // namespace cosmocad::app
