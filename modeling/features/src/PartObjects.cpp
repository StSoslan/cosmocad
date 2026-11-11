#include "cosmocad/modeling/PartObjects.h"

#include <nlohmann/json.hpp>

namespace cosmocad::modeling {

using core::ErrorCode;
using core::makeError;
using nlohmann::json;

namespace {

template <class F>
Result<void> guarded(const char* what, F&& body) {
    try {
        body();
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string(what) + ": " + e.what());
    }
}

const char* shapeKindName(SketchShape::Kind kind) {
    switch (kind) {
        case SketchShape::Kind::Rectangle: return "rectangle";
        case SketchShape::Kind::Circle: return "circle";
        case SketchShape::Kind::Polygon: return "polygon";
    }
    return "rectangle";
}

}  // namespace

const char* toString(FeatureKind kind) {
    switch (kind) {
        case FeatureKind::Extrude: return "extrude";
        case FeatureKind::Revolve: return "revolve";
        case FeatureKind::Fillet: return "fillet";
        case FeatureKind::Chamfer: return "chamfer";
        case FeatureKind::Mirror: return "mirror";
        case FeatureKind::Sweep: return "sweep";
    }
    return "extrude";
}

void Parameter::writeJson(json& j) const {
    j["expression"] = expression;
    if (!comment.empty()) j["comment"] = comment;
}

Result<void> Parameter::readJson(const json& j) {
    return guarded("параметр", [&] {
        expression = j.at("expression").get<std::string>();
        comment = j.value("comment", std::string());
    });
}

void Sketch::writeJson(json& j) const {
    j["plane"] = plane;
    j["offset"] = offset;
    if (!faceName.empty()) j["faceName"] = faceName;
    j["nextTag"] = nextTag;
    json list = json::array();
    for (const auto& shape : shapes)
        list.push_back({{"kind", shapeKindName(shape.kind)}, {"tag", shape.tag}, {"hole", shape.hole},
                        {"values", shape.values}});
    j["shapes"] = std::move(list);
    if (!sourceDrawingId.isNil()) {
        j["sourceDrawing"] = sourceDrawingId.toString();
        json ids = json::array();
        for (const auto& id : sourceEntities) ids.push_back(id.toString());
        j["sourceEntities"] = std::move(ids);
    }
}

Result<void> Sketch::readJson(const json& j) {
    bool badKind = false;
    auto parsed = guarded("эскиз", [&] {
        plane = j.value("plane", std::string("XY"));
        offset = j.value("offset", std::string("0"));
        faceName = j.value("faceName", std::string());
        nextTag = j.value("nextTag", std::uint64_t{1});
        shapes.clear();
        for (const auto& item : j.at("shapes")) {
            SketchShape shape;
            const std::string kind = item.at("kind").get<std::string>();
            if (kind == "rectangle")
                shape.kind = SketchShape::Kind::Rectangle;
            else if (kind == "circle")
                shape.kind = SketchShape::Kind::Circle;
            else if (kind == "polygon")
                shape.kind = SketchShape::Kind::Polygon;
            else
                badKind = true;
            shape.tag = item.at("tag").get<std::uint64_t>();
            shape.hole = item.value("hole", false);
            shape.values = item.at("values").get<std::vector<std::string>>();
            shapes.push_back(std::move(shape));
        }
        sourceDrawingId = core::Uuid();
        sourceEntities.clear();
        if (j.contains("sourceDrawing")) {
            if (auto id = core::Uuid::parse(j.at("sourceDrawing").get<std::string>()))
                sourceDrawingId = id.value();
            else
                badKind = true;
            for (const auto& item : j.value("sourceEntities", json::array()))
                if (auto id = core::Uuid::parse(item.get<std::string>())) sourceEntities.push_back(id.value());
        }
    });
    if (!parsed) return parsed;
    if (badKind) return makeError(ErrorCode::ParseError, "эскиз: неизвестный вид фигуры");
    return {};
}

void Feature::writeJson(json& j) const {
    j["kind"] = toString(kind);
    j["tag"] = tag;
    j["sequence"] = sequence;
    j["suppressed"] = suppressed;
    j["value"] = value;
    if (kind == FeatureKind::Sweep) j["path"] = path;
    if (kind == FeatureKind::Extrude || kind == FeatureKind::Revolve || kind == FeatureKind::Sweep) {
        j["sketchId"] = sketchId.toString();
        j["cut"] = cut;
    }
    if (kind == FeatureKind::Extrude) j["reversed"] = reversed;
    if (pattern.kind != Pattern::Kind::None)
        j["pattern"] = {{"kind", pattern.kind == Pattern::Kind::Linear ? "linear" : "circular"},
                        {"count", pattern.count},     {"stepX", pattern.stepX},     {"stepY", pattern.stepY},
                        {"centerX", pattern.centerX}, {"centerY", pattern.centerY}, {"angle", pattern.angle}};
    if (kind == FeatureKind::Revolve || kind == FeatureKind::Mirror) j["axis"] = axis;
    if (kind == FeatureKind::Fillet || kind == FeatureKind::Chamfer) {
        json list = json::array();
        for (const auto& edge : edges) list.push_back(json::array({edge.faceA, edge.faceB}));
        j["edges"] = std::move(list);
    }
}

Result<void> Feature::readJson(const json& j) {
    bool bad = false;
    auto parsed = guarded("операция", [&] {
        const std::string name = j.at("kind").get<std::string>();
        if (name == "extrude")
            kind = FeatureKind::Extrude;
        else if (name == "revolve")
            kind = FeatureKind::Revolve;
        else if (name == "fillet")
            kind = FeatureKind::Fillet;
        else if (name == "chamfer")
            kind = FeatureKind::Chamfer;
        else if (name == "mirror")
            kind = FeatureKind::Mirror;
        else if (name == "sweep")
            kind = FeatureKind::Sweep;
        else
            bad = true;
        tag = j.at("tag").get<std::string>();
        sequence = j.value("sequence", std::int64_t{0});
        suppressed = j.value("suppressed", false);
        value = j.value("value", std::string("10"));
        cut = j.value("cut", false);
        reversed = j.value("reversed", false);
        axis = j.value("axis", std::string("Y"));
        path = j.value("path", std::vector<std::array<std::string, 3>>{});
        if (j.contains("sketchId")) {
            auto id = core::Uuid::parse(j.at("sketchId").get<std::string>());
            if (id)
                sketchId = id.value();
            else
                bad = true;
        }
        pattern = Pattern{};
        if (j.contains("pattern")) {
            const auto& p = j.at("pattern");
            const std::string patternKind = p.at("kind").get<std::string>();
            if (patternKind == "linear")
                pattern.kind = Pattern::Kind::Linear;
            else if (patternKind == "circular")
                pattern.kind = Pattern::Kind::Circular;
            else
                bad = true;
            pattern.count = p.value("count", std::string("1"));
            pattern.stepX = p.value("stepX", std::string("0"));
            pattern.stepY = p.value("stepY", std::string("0"));
            pattern.centerX = p.value("centerX", std::string("0"));
            pattern.centerY = p.value("centerY", std::string("0"));
            pattern.angle = p.value("angle", std::string("360"));
        }
        edges.clear();
        if (j.contains("edges"))
            for (const auto& item : j.at("edges"))
                edges.push_back({item.at(0).get<std::string>(), item.at(1).get<std::string>()});
    });
    if (!parsed) return parsed;
    if (bad) return makeError(ErrorCode::ParseError, "операция: неизвестный вид или неверная ссылка на эскиз");
    return {};
}

void PartProperties::writeJson(json& j) const {
    j["designation"] = designation;
    j["material"] = material;
    j["density"] = density;
    if (!rollbackAfter.isNil()) j["rollbackAfter"] = rollbackAfter.toString();
}

Result<void> PartProperties::readJson(const json& j) {
    return guarded("свойства детали", [&] {
        designation = j.value("designation", std::string());
        material = j.value("material", std::string());
        density = j.value("density", 7.85);
        rollbackAfter = core::Uuid();
        if (j.contains("rollbackAfter"))
            if (auto id = core::Uuid::parse(j.at("rollbackAfter").get<std::string>())) rollbackAfter = id.value();
    });
}

void Configuration::writeJson(json& j) const {
    j["values"] = values;
    if (!designation.empty()) j["designation"] = designation;
}

Result<void> Configuration::readJson(const json& j) {
    return guarded("исполнение", [&] {
        values = j.at("values").get<std::map<std::string, std::string>>();
        designation = j.value("designation", std::string());
    });
}

void registerTypes(document::ObjectFactory& factory) {
    factory.registerType<Configuration>();
    factory.registerType<Parameter>();
    factory.registerType<Sketch>();
    factory.registerType<Feature>();
    factory.registerType<PartProperties>();
}

}  // namespace cosmocad::modeling
