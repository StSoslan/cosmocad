#include "cosmocad/standards/Engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace cosmocad::standards {

using document::Document;
using drawing::Entity;
using drawing::Layer;
using drawing::Sheet;
using geometry::BBox2;

std::optional<SheetGeometry> sheetGeometry(const Sheet& sheet, const Profile& profile) {
    const SheetFormat* format = profile.findFormat(sheet.format);
    if (!format) return std::nullopt;
    SheetGeometry g;
    g.width = sheet.landscape ? format->height : format->width;
    g.height = sheet.landscape ? format->width : format->height;
    g.frame = BBox2::fromCorners({profile.frame.left, profile.frame.bottom},
                                 {g.width - profile.frame.right, g.height - profile.frame.top});
    g.titleBlock = BBox2::fromCorners({g.frame.max.x - profile.titleBlock.width, g.frame.min.y},
                                      {g.frame.max.x, g.frame.min.y + profile.titleBlock.height});
    return g;
}

double mainLineWidthOf(const Document& document) {
    for (const auto& object : document.objects())
        if (auto* sheet = dynamic_cast<const Sheet*>(object.get())) return sheet->mainLineWidth;
    return 0.8;
}

ResolvedLineStyle resolveLineStyle(const Document& document, const Entity& entity, const Profile& profile,
                                   double mainWidth) {
    ResolvedLineStyle style;
    const Layer* layer = entity.layerId.isNil() ? nullptr : document.findAs<Layer>(entity.layerId);

    style.lineTypeId = entity.lineTypeId ? *entity.lineTypeId : (layer ? layer->lineTypeId : "solid_main");
    if (layer) {
        style.color = layer->color;
        style.visible = layer->visible;
    }

    const LineType* type = profile.findLineType(style.lineTypeId);
    style.knownLineType = type != nullptr;
    LineClass lineClass = type ? type->lineClass : LineClass::Main;
    if (type) style.pattern = type->pattern;

    if (entity.lineWidth)
        style.width = *entity.lineWidth;
    else if (layer && layer->lineWidth > 0.0)
        style.width = layer->lineWidth;
    else
        style.width = lineClass == LineClass::Main ? mainWidth : mainWidth / 2.0;
    return style;
}

namespace {

struct Context {
    const Document& document;
    const Profile& profile;
    const RuleSpec& rule;
    std::vector<Violation>& out;

    void report(const core::Uuid& objectId, const std::string& detail = {}) const {
        Violation v;
        v.ruleId = rule.id;
        v.severity = rule.severity;
        v.objectId = objectId;
        v.message = rule.message;
        if (!detail.empty()) v.message += (v.message.empty() ? "" : " ") + detail;
        v.reference = rule.reference;
        out.push_back(std::move(v));
    }

    const Sheet* sheet() const {
        auto sheets = document.objectsOf<Sheet>();
        return sheets.empty() ? nullptr : sheets.front();
    }
};

std::string number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3g", value);
    return buffer;
}

void ruleSheetPresent(const Context& c) {
    auto count = c.document.objectsOf<Sheet>().size();
    if (count != 1) c.report({}, "(листов в документе: " + std::to_string(count) + ")");
}

void ruleSheetFormatKnown(const Context& c) {
    if (const Sheet* sheet = c.sheet(); sheet && !c.profile.findFormat(sheet->format))
        c.report(sheet->id(), "(формат «" + sheet->format + "»)");
}

void ruleMainLineWidthRange(const Context& c) {
    const Sheet* sheet = c.sheet();
    if (!sheet) return;
    double min = c.rule.params.value("min", 0.0);
    double max = c.rule.params.value("max", 1e9);
    if (sheet->mainLineWidth < min - 1e-9 || sheet->mainLineWidth > max + 1e-9)
        c.report(sheet->id(), "(задано " + number(sheet->mainLineWidth) + " мм, допустимо " + number(min) + "–" +
                                  number(max) + " мм)");
}

void ruleLineWidthByClass(const Context& c) {
    const Sheet* sheet = c.sheet();
    double s = sheet ? sheet->mainLineWidth : 0.8;
    double ratioMin = c.rule.params.value("thinRatioMin", 1.0 / 3.0);
    double ratioMax = c.rule.params.value("thinRatioMax", 0.5);
    const double eps = 1e-6;
    // Виды объектов, толщину линий которых задаёт их собственный стандарт
    // (общий знак шероховатости в углу листа — в 1,5 раза толще, ГОСТ 2.309-73, п. 2.6).
    const auto ownWidth = c.rule.params.value("ownWidthTypes", std::vector<std::string>{});
    for (const Entity* entity : c.document.objectsOf<Entity>()) {
        if (entity->lineWidth &&
            std::find(ownWidth.begin(), ownWidth.end(), std::string(entity->typeName())) != ownWidth.end())
            continue;
        auto style = resolveLineStyle(c.document, *entity, c.profile, s);
        const LineType* type = c.profile.findLineType(style.lineTypeId);
        if (!type) continue;  // неизвестный тип линии ловит отдельное правило
        if (type->lineClass == LineClass::Main) {
            if (std::abs(style.width - s) > eps)
                c.report(entity->id(), "(основная линия " + number(style.width) + " мм при s = " + number(s) + " мм)");
        } else {
            double ratio = style.width / s;
            if (ratio < ratioMin - eps || ratio > ratioMax + eps)
                c.report(entity->id(), "(тонкая линия " + number(style.width) + " мм при s = " + number(s) + " мм)");
        }
    }
}

void ruleKnownLineType(const Context& c) {
    const double s = mainLineWidthOf(c.document);
    for (const Entity* entity : c.document.objectsOf<Entity>()) {
        auto style = resolveLineStyle(c.document, *entity, c.profile, s);
        if (!style.knownLineType) c.report(entity->id(), "(тип линии «" + style.lineTypeId + "»)");
    }
}

void ruleTextHeightAllowed(const Context& c) {
    auto allowed = [&](double height) {
        for (double h : c.profile.textHeights)
            if (std::abs(h - height) < 1e-6) return true;
        return false;
    };
    for (const Entity* entity : c.document.objectsOf<Entity>())
        if (auto height = entity->textHeightValue(); height && !allowed(*height))
            c.report(entity->id(), "(высота " + number(*height) + " мм)");
}

void ruleInsideFrame(const Context& c) {
    const Sheet* sheet = c.sheet();
    if (!sheet) return;
    auto g = sheetGeometry(*sheet, c.profile);
    if (!g) return;
    for (const Entity* entity : c.document.objectsOf<Entity>())
        if (!g->frame.contains(entity->bounds(), 1e-6)) c.report(entity->id());
}

void ruleTitleBlockRequired(const Context& c) {
    const Sheet* sheet = c.sheet();
    if (!sheet) return;
    for (const auto& field : c.profile.titleBlock.requiredFields) {
        auto it = sheet->titleBlock.find(field);
        if (it == sheet->titleBlock.end() || it->second.empty()) c.report(sheet->id(), "(поле «" + field + "»)");
    }
}

void ruleDegenerateGeometry(const Context& c) {
    for (const auto* line : c.document.objectsOf<drawing::LineEntity>())
        if (geometry::distance(line->start, line->end) < geometry::kLinearTolerance) c.report(line->id());
    for (const auto* circle : c.document.objectsOf<drawing::CircleEntity>())
        if (circle->radius < geometry::kLinearTolerance) c.report(circle->id());
    for (const auto* arc : c.document.objectsOf<drawing::ArcEntity>())
        if (arc->radius < geometry::kLinearTolerance) c.report(arc->id());
}

void ruleHatchSpacingRange(const Context& c) {
    double min = c.rule.params.value("min", 0.0);
    double max = c.rule.params.value("max", 1e9);
    for (const auto* hatch : c.document.objectsOf<drawing::Hatch>())
        if (hatch->spacing < min - 1e-9 || hatch->spacing > max + 1e-9)
            c.report(hatch->id(), "(шаг " + number(hatch->spacing) + " мм, допустимо " + number(min) + "–" + number(max) + " мм)");
}

void ruleHatchAngleAllowed(const Context& c) {
    // Допустимые наклоны заданы в градусах к горизонтали; наклон влево и вправо равноправны.
    auto allowed = c.rule.params.value("degrees", std::vector<double>{45.0});
    for (const auto* hatch : c.document.objectsOf<drawing::Hatch>()) {
        double degrees = std::fmod(std::abs(hatch->angle) * 180.0 / 3.14159265358979323846, 180.0);
        if (degrees > 90.0) degrees = 180.0 - degrees;
        bool ok = false;
        for (double a : allowed) ok = ok || std::abs(a - degrees) < 0.01;
        if (!ok) c.report(hatch->id(), "(наклон " + number(degrees) + "°)");
    }
}

const std::map<std::string, std::function<void(const Context&)>>& ruleTable() {
    static const std::map<std::string, std::function<void(const Context&)>> table = {
        {"sheet_present", ruleSheetPresent},
        {"sheet_format_known", ruleSheetFormatKnown},
        {"main_line_width_range", ruleMainLineWidthRange},
        {"line_width_by_class", ruleLineWidthByClass},
        {"known_line_type", ruleKnownLineType},
        {"text_height_allowed", ruleTextHeightAllowed},
        {"inside_frame", ruleInsideFrame},
        {"title_block_required", ruleTitleBlockRequired},
        {"degenerate_geometry", ruleDegenerateGeometry},
        {"hatch_spacing_range", ruleHatchSpacingRange},
        {"hatch_angle_allowed", ruleHatchAngleAllowed},
    };
    return table;
}

}  // namespace

std::vector<Violation> checkDocument(const Document& document, const Profile& profile) {
    std::vector<Violation> out;
    for (const RuleSpec& rule : profile.rules) {
        Context context{document, profile, rule, out};
        auto it = ruleTable().find(rule.type);
        if (it == ruleTable().end()) {
            // Опечатка в пользовательском профиле не должна молча отключать проверку.
            Violation v;
            v.ruleId = rule.id;
            v.severity = Severity::Warning;
            v.message = "Правило не выполнено: неизвестный вид проверки «" + rule.type + "»";
            out.push_back(std::move(v));
            continue;
        }
        it->second(context);
    }
    return out;
}

Result<std::vector<Violation>> StandardsService::check(const Document& document) const {
    if (document.profileId.empty())
        return core::makeError(core::ErrorCode::InvalidState, "для документа не выбран профиль стандарта");
    auto resolved = registry_.resolve(document.profileId);
    if (!resolved) return resolved.error();
    return checkDocument(document, resolved.value());
}

}  // namespace cosmocad::standards
