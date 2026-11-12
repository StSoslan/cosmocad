// 3D-часть общей «сборки» модулей: связь деталей, сборок и чертежей между собой.
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>

#include "AppContext.h"
#include "cosmocad/modeling/Expression.h"

namespace cosmocad::app {

using core::ErrorCode;
using core::makeError;
using core::Result;
using core::Uuid;
using document::Document;
using document::DocumentKind;
using geometry::Point2;
using geometry::ShapePtr;
using geometry::Vector3;
using modeling::SketchShape;

// ---- Детали для сборки ----

Result<ShapePtr> AppContext::PartSource::bodyOf(const Uuid& documentId) {
    Document* document = context_.documents().findDocument(documentId);
    if (!document) return makeError(ErrorCode::NotFound, "документ детали не найден в проекте");
    if (document->kind() == DocumentKind::Assembly) {
        // Подсборка: все её компоненты на своих местах, как одно целое.
        if (std::find(building_.begin(), building_.end(), documentId) != building_.end())
            return makeError(ErrorCode::InvalidState, "сборка «" + document->name + "» оказалась вложена сама в себя");
        building_.push_back(documentId);
        auto placed = context_.assembly().placedBodies(*document);
        building_.pop_back();
        if (!placed) return placed.error();
        std::vector<ShapePtr> shapes;
        for (const auto& item : placed.value()) shapes.push_back(item.shape);
        if (shapes.empty()) return makeError(ErrorCode::InvalidState, "в сборке «" + document->name + "» нет компонентов");
        return context_.kernel().combine(shapes);
    }
    if (document->kind() != DocumentKind::Part)
        return makeError(ErrorCode::InvalidArgument, "компонентом сборки может быть деталь или другая сборка");
    // Перестроение берёт готовое, если ничего не менялось, поэтому вызывать его можно каждый раз.
    auto report = context_.modeling().recompute(*document);
    if (!report) return report.error();
    ShapePtr body = context_.modeling().body(*document);
    if (!body) return makeError(ErrorCode::InvalidState, "у детали «" + document->name + "» нет тела");
    return body;
}

Result<assembly::PartInfo> AppContext::PartSource::infoOf(const Uuid& documentId) {
    Document* document = context_.documents().findDocument(documentId);
    if (!document) return makeError(ErrorCode::NotFound, "документ детали не найден в проекте");
    assembly::PartInfo info;
    info.name = document->name;
    if (document->kind() == DocumentKind::Assembly) {
        // Масса подсборки — сумма масс её состава.
        if (std::find(building_.begin(), building_.end(), documentId) != building_.end())
            return makeError(ErrorCode::InvalidState, "сборка «" + document->name + "» оказалась вложена сама в себя");
        building_.push_back(documentId);
        auto lines = context_.assembly().billOfMaterials(*document);
        building_.pop_back();
        if (!lines) return lines.error();
        for (const auto& line : lines.value()) info.massGrams += line.massGrams * line.quantity;
        info.material = "сборочная единица";
        return info;
    }
    if (document->kind() != DocumentKind::Part)
        return makeError(ErrorCode::InvalidArgument, "компонентом сборки может быть деталь или другая сборка");
    if (const auto list = document->objectsOf<modeling::PartProperties>(); !list.empty()) {
        info.designation = list.front()->designation;
        info.material = list.front()->material;
    }
    if (auto body = bodyOf(documentId))
        if (auto mass = context_.modeling().mass(*document)) info.massGrams = mass.value().massGrams;
    return info;
}

Result<std::vector<ShapePtr>> AppContext::bodiesOf(Document& document, bool exploded) {
    std::vector<ShapePtr> out;
    if (document.kind() == DocumentKind::Part) {
        auto body = partSource_->bodyOf(document.id());
        if (!body) return body.error();
        out.push_back(body.value());
    } else if (document.kind() == DocumentKind::Assembly) {
        auto placed = assembly_->placedBodies(document, exploded);
        if (!placed) return placed.error();
        for (const auto& item : placed.value()) out.push_back(item.shape);
        if (out.empty()) return makeError(ErrorCode::InvalidState, "в сборке нет компонентов");
    } else {
        return makeError(ErrorCode::InvalidArgument, "у документа «" + document.name + "» нет 3D-тел");
    }
    return out;
}

Result<void> AppContext::rebuildAll() {
    for (const auto& document : documents().project().documents())
        if (document->kind() == DocumentKind::Part) {
            if (auto synced = syncSketches(*document); !synced) return synced.error();
            if (auto report = modeling_->recompute(*document); !report) return report.error();
        }
    return {};
}

// ---- Виды чертежа по модели ----

namespace {

struct ViewDirection {
    const char* name;
    Vector3 direction;  // куда смотрит наблюдатель
    Vector3 up;
};

const ViewDirection* findView(const std::string& name) {
    static const ViewDirection views[] = {
        {"front", {0, 1, 0}, {0, 0, 1}},  {"back", {0, -1, 0}, {0, 0, 1}}, {"left", {1, 0, 0}, {0, 0, 1}},
        {"right", {-1, 0, 0}, {0, 0, 1}}, {"top", {0, 0, -1}, {0, 1, 0}},  {"bottom", {0, 0, 1}, {0, -1, 0}},
    };
    for (const auto& view : views)
        if (name == view.name) return &view;
    return nullptr;
}

double dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Где показать секущую плоскость разреза: первый обычный вид той же модели, на котором
// плоскость видна линией (взгляд вида перпендикулярен взгляду разреза).
std::optional<drawing::SectionMark> sectionMark(const Document& sheet, const Uuid& sourceId,
                                                const ViewDirection& section, double at) {
    for (const auto* parent : sheet.objectsOf<drawing::ModelView>()) {
        if (parent->section || parent->sourceDocumentId != sourceId) continue;
        const ViewDirection* direction = findView(parent->view);
        if (!direction || std::abs(dot(direction->direction, section.direction)) > 1e-9) continue;
        const Vector3 toEye{-direction->direction.x, -direction->direction.y, -direction->direction.z};
        const Vector3 right = geometry::normalized(geometry::crossProduct(direction->up, toEye));
        const Vector3 upward = geometry::crossProduct(toEye, right);
        geometry::BBox2 box;
        for (const auto& id : parent->entities)
            if (const auto* entity = sheet.findAs<drawing::Entity>(id)) {
                box.expand(entity->bounds());
            }
        if (!box.valid) continue;
        const double alongRight = dot(section.direction, right), alongUp = dot(section.direction, upward);
        drawing::SectionMark mark;
        if (std::abs(alongRight) > 0.5) {  // след плоскости — вертикальная линия
            const double x = parent->origin.x + at / alongRight * parent->scale;
            mark.a = {x, box.max.y + 4.0};
            mark.b = {x, box.min.y - 4.0};
            mark.direction = {alongRight > 0 ? 1.0 : -1.0, 0.0};
        } else {
            const double y = parent->origin.y + at / alongUp * parent->scale;
            mark.a = {box.min.x - 4.0, y};
            mark.b = {box.max.x + 4.0, y};
            mark.direction = {0.0, alongUp > 0 ? 1.0 : -1.0};
        }
        return mark;
    }
    return std::nullopt;
}

// Следующая свободная буква разреза. Буквы Ё, З, Й, О, Х, Ч, Ъ, Ы, Ь не применяются (ГОСТ 2.305-2008).
std::string nextSectionLabel(const Document& sheet) {
    static const char* letters[] = {"А", "Б", "В", "Г", "Д", "Е", "Ж", "И", "К", "Л", "М", "Н", "П", "Р",
                                    "С", "Т", "У", "Ф", "Ц", "Ш", "Щ", "Э", "Ю", "Я"};
    for (const char* letter : letters) {
        const std::string label = std::string(letter) + "–" + letter;
        bool used = false;
        for (const auto* view : sheet.objectsOf<drawing::ModelView>()) used = used || view->label == label;
        if (!used) return label;
    }
    return "А1–А1";
}

}  // namespace

Result<Uuid> AppContext::addModelView(Document& drawingDocument, Document& source, const std::string& view,
                                      Point2 center, double scale, bool showHidden, bool exploded) {
    const ViewDirection* direction = findView(view);
    if (!direction) return makeError(ErrorCode::InvalidArgument, "неизвестный вид «" + view + "»");
    auto bodies = bodiesOf(source, exploded);
    if (!bodies) return bodies.error();
    auto whole = kernel_->combine(bodies.value());
    if (!whole) return whole.error();
    auto projected = kernel_->project(whole.value(), direction->direction, direction->up, showHidden);
    if (!projected) return projected.error();
    drawing::ModelViewParams params;
    params.sourceDocumentId = source.id();
    params.view = view;
    params.center = center;
    params.scale = scale;
    params.showHidden = showHidden;
    params.exploded = exploded;
    return drawing().addModelView(drawingDocument, params, projected.value());
}

// Тела для разреза: что рассекается и что показывается целым (компоненты с пометкой «не рассекать»).
Result<std::pair<ShapePtr, ShapePtr>> AppContext::sectionBodies(Document& source) {
    std::vector<ShapePtr> cut, whole;
    if (source.kind() == DocumentKind::Assembly) {
        auto placed = assembly_->placedBodies(source);
        if (!placed) return placed.error();
        for (const auto& item : placed.value()) (item.wholeInSection ? whole : cut).push_back(item.shape);
    } else {
        auto bodies = bodiesOf(source);
        if (!bodies) return bodies.error();
        cut = bodies.take();
    }
    if (cut.empty()) return makeError(ErrorCode::InvalidState, "рассекать нечего: все компоненты помечены «не рассекать»");
    auto cutShape = kernel_->combine(cut);
    if (!cutShape) return cutShape.error();
    ShapePtr wholeShape;
    if (!whole.empty()) {
        auto combined = kernel_->combine(whole);
        if (!combined) return combined.error();
        wholeShape = combined.value();
    }
    return std::pair<ShapePtr, ShapePtr>{cutShape.value(), wholeShape};
}

// Линии вида или разреза по нынешнему состоянию модели и место обозначения секущей плоскости.
Result<AppContext::ViewProjection> AppContext::projectView(const Document& drawingDocument, const drawing::ModelView& view,
                                                           double at) {
    Document* source = documents().findDocument(view.sourceDocumentId);
    if (!source) return makeError(ErrorCode::NotFound, "модель, по которой построен вид, не найдена в проекте");
    const ViewDirection* direction = findView(view.view);
    if (!direction) return makeError(ErrorCode::InvalidState, "неизвестный вид «" + view.view + "»");
    const Vector3 d = direction->direction;
    ViewProjection out;
    if (view.section) {
        auto parts = sectionBodies(*source);
        if (!parts) return parts.error();
        auto projected = kernel_->projectSection(parts.value().first, d, direction->up, {d.x * at, d.y * at, d.z * at},
                                                 view.showHidden, parts.value().second, view.sectionOnly);
        if (!projected) return projected.error();
        out.projected = projected.take();
        out.mark = sectionMark(drawingDocument, view.sourceDocumentId, *direction, at);
    } else {
        auto bodies = bodiesOf(*source, view.exploded);
        if (!bodies) return bodies.error();
        auto whole = kernel_->combine(bodies.value());
        if (!whole) return whole.error();
        auto projected = kernel_->project(whole.value(), d, direction->up, view.showHidden);
        if (!projected) return projected.error();
        out.projected = projected.take();
    }
    return out;
}

Result<Uuid> AppContext::addSectionView(Document& drawingDocument, Document& source, const std::string& view,
                                        Point2 center, double scale, std::optional<double> at, bool onlyCut) {
    const ViewDirection* direction = findView(view);
    if (!direction) return makeError(ErrorCode::InvalidArgument, "неизвестный вид «" + view + "»");
    const Vector3 d = direction->direction;
    if (!at) {
        auto bodies = bodiesOf(source);
        if (!bodies) return bodies.error();
        auto whole = kernel_->combine(bodies.value());
        if (!whole) return whole.error();
        auto box = kernel_->bounds(whole.value());
        if (!box) return box.error();
        const Vector3 lo{box.value().min.x, box.value().min.y, box.value().min.z};
        const Vector3 hi{box.value().max.x, box.value().max.y, box.value().max.z};
        at = (dot(lo, d) + dot(hi, d)) / 2.0;
    }
    drawing::ModelView probe;
    probe.sourceDocumentId = source.id();
    probe.view = view;
    probe.section = true;
    probe.sectionOnly = onlyCut;
    auto projection = projectView(drawingDocument, probe, *at);
    if (!projection) return projection.error();
    drawing::ModelViewParams params;
    params.sourceDocumentId = source.id();
    params.view = view;
    params.center = center;
    params.scale = scale;
    params.section = true;
    params.sectionOnly = onlyCut;
    params.sectionAt = *at;
    params.label = nextSectionLabel(drawingDocument);
    params.mark = projection.value().mark;
    return drawing().addModelView(drawingDocument, params, projection.value().projected);
}

Result<void> AppContext::moveSection(Document& drawingDocument, const Uuid& viewId, double at) {
    const auto* view = drawingDocument.findAs<drawing::ModelView>(viewId);
    if (!view || !view->section) return makeError(ErrorCode::NotFound, "разрез не найден");
    auto projection = projectView(drawingDocument, *view, at);
    if (!projection) return projection.error();
    auto moved = drawing().updateModelView(drawingDocument, viewId, projection.value().projected, projection.value().mark, at);
    if (!moved) return moved.error();
    lastViewUpdate_ = moved.value();
    return {};
}

Result<int> AppContext::staleViewCount(Document& drawingDocument) {
    int stale = 0;
    for (const auto* view : drawingDocument.objectsOf<drawing::ModelView>()) {
        if (view->stamp.empty()) continue;  // вид из старого файла: отпечатка нет, судить не по чему
        auto projection = projectView(drawingDocument, *view, view->sectionAt);
        // Вид, который больше нельзя построить (модель удалена, плоскость ушла мимо тела), тоже устарел.
        if (!projection || drawing::viewStamp(projection.value().projected) != view->stamp) ++stale;
    }
    return stale;
}

Result<int> AppContext::updateModelViews(Document& drawingDocument) {
    int updated = 0;
    lastViewUpdate_ = {};
    // Список снимается заранее: обновление меняет документ.
    // Сначала обычные виды, потом разрезы: обозначение секущей плоскости ставится по уже обновлённому виду.
    std::vector<Uuid> ids;
    for (const bool sections : {false, true})
        for (const auto* view : drawingDocument.objectsOf<drawing::ModelView>())
            if (view->section == sections) ids.push_back(view->id());
    for (const auto& id : ids) {
        const auto* view = drawingDocument.findAs<drawing::ModelView>(id);
        auto projection = projectView(drawingDocument, *view, view->sectionAt);
        if (!projection) return projection.error();
        auto moved = drawing().updateModelView(drawingDocument, id, projection.value().projected, projection.value().mark);
        if (!moved) return moved.error();
        lastViewUpdate_.movedDimensions += moved.value().movedDimensions;
        lastViewUpdate_.lostDimensions += moved.value().lostDimensions;
        ++updated;
    }
    return updated;
}

// ---- Параметры детали → ограничения на чертеже → эскиз детали ----

Result<double> AppContext::evaluateWith(const Document& part, const std::string& expression) const {
    if (part.kind() != DocumentKind::Part) return makeError(ErrorCode::InvalidArgument, "параметры берутся из документа «Деталь»");
    auto values = modeling_->parameterValues(part);
    if (!values) return values.error();
    return modeling::evaluateExpression(expression, [&](std::string_view name) -> std::optional<double> {
        const auto it = values.value().find(std::string(name));
        if (it == values.value().end()) return std::nullopt;
        return it->second;
    });
}

Result<int> AppContext::syncSketches(Document& part) {
    if (part.kind() != DocumentKind::Part) return 0;
    // 1. Формулы ограничений, которые берут параметры из этой детали.
    for (const auto& document : documents().project().documents()) {
        if (document->kind() != DocumentKind::Drawing) continue;
        std::map<Uuid, double> changed;
        for (const auto* constraint : document->objectsOf<sketcher::Constraint>()) {
            if (constraint->expression.empty() || constraint->parameterDocumentId != part.id()) continue;
            auto value = evaluateWith(part, constraint->expression);
            if (!value)
                return makeError(value.error().code, "ограничение «" + constraint->name + "» на чертеже «" + document->name +
                                                         "»: " + value.error().message);
            if (std::abs(value.value() - constraint->value) > 1e-12) changed[constraint->id()] = value.value();
        }
        if (changed.empty()) continue;
        if (auto moved = sketcher_.setValues(*document, changed, true); !moved)
            return makeError(moved.error().code, "чертёж «" + document->name + "»: " + moved.error().message);
    }
    // 2. Эскизы детали, снятые с контуров чертежей.
    int updated = 0;
    std::vector<Uuid> sketchIds;
    for (const auto* sketch : part.objectsOf<modeling::Sketch>())
        if (!sketch->sourceDrawingId.isNil()) sketchIds.push_back(sketch->id());
    for (const auto& id : sketchIds) {
        const auto* sketch = part.findAs<modeling::Sketch>(id);
        const Document* drawing = documents().findDocument(sketch->sourceDrawingId);
        if (!drawing) continue;  // чертёж удалён: эскиз остаётся каким был
        auto shapes = contourFromDrawing(*drawing, sketch->sourceEntities);
        if (!shapes) continue;   // контур разомкнут или элементы удалены: эскиз остаётся каким был
        // Прежние метки сохраняются, чтобы имена граней тела не менялись.
        bool same = shapes.value().size() == sketch->shapes.size();
        for (std::size_t i = 0; i < shapes.value().size(); ++i) {
            if (i < sketch->shapes.size() && sketch->shapes[i].kind == shapes.value()[i].kind) shapes.value()[i].tag = sketch->shapes[i].tag;
            same = same && i < sketch->shapes.size() && sketch->shapes[i].kind == shapes.value()[i].kind &&
                   sketch->shapes[i].hole == shapes.value()[i].hole && sketch->shapes[i].values == shapes.value()[i].values;
        }
        if (same) continue;
        if (auto ok = modeling_->updateSketch(part, id, shapes.value()); !ok) return ok.error();
        ++updated;
    }
    return updated;
}

// ---- Контур из чертежа ----

namespace {

struct Loop {
    bool circle = false;
    Point2 center;
    double radius = 0.0;
    std::vector<Point2> points;

    // Точка на самой линии контура: по ней видно, лежит ли контур внутри другого.
    Point2 reference() const { return circle ? Point2{center.x + radius, center.y} : points.front(); }
    bool contains(Point2 p) const {
        if (circle) return geometry::distance(p, center) < radius;
        bool inside = false;
        for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const Point2 a = points[i], b = points[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside;
    }
};

}  // namespace

Result<std::vector<SketchShape>> AppContext::contourFromDrawing(const Document& drawingDocument,
                                                                const std::vector<Uuid>& ids) {
    std::vector<Loop> loops;
    std::vector<std::pair<Point2, Point2>> segments;
    for (const auto& id : ids) {
        if (const auto* circle = drawingDocument.findAs<drawing::CircleEntity>(id)) {
            Loop loop;
            loop.circle = true;
            loop.center = circle->center;
            loop.radius = circle->radius;
            loops.push_back(loop);
        } else if (const auto* line = drawingDocument.findAs<drawing::LineEntity>(id)) {
            segments.emplace_back(line->start, line->end);
        } else if (drawingDocument.findAs<drawing::ArcEntity>(id)) {
            return makeError(ErrorCode::NotImplemented,
                             "контур с дугами пока не поддерживается: выберите отрезки и окружности");
        }
        // Размеры, тексты и прочее просто не участвуют в контуре.
    }
    const double tolerance = 1e-4;
    while (!segments.empty()) {
        Loop loop;
        loop.points.push_back(segments.back().first);
        Point2 end = segments.back().second;
        segments.pop_back();
        while (!geometry::almostEqual(end, loop.points.front(), tolerance)) {
            loop.points.push_back(end);
            auto next = std::find_if(segments.begin(), segments.end(), [&](const auto& s) {
                return geometry::almostEqual(s.first, end, tolerance) || geometry::almostEqual(s.second, end, tolerance);
            });
            if (next == segments.end())
                return makeError(ErrorCode::InvalidArgument,
                                 "контур не замкнут: концы отрезков должны сходиться точно (включите привязку)");
            end = geometry::almostEqual(next->first, end, tolerance) ? next->second : next->first;
            segments.erase(next);
        }
        if (loop.points.size() < 3) return makeError(ErrorCode::InvalidArgument, "в контуре меньше трёх отрезков");
        loops.push_back(std::move(loop));
    }
    if (loops.empty())
        return makeError(ErrorCode::InvalidArgument, "выберите на чертеже замкнутый контур из отрезков или окружность");

    std::vector<SketchShape> shapes;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        int depth = 0;
        for (std::size_t j = 0; j < loops.size(); ++j)
            if (i != j && loops[j].contains(loops[i].reference())) ++depth;
        SketchShape shape;
        shape.hole = depth % 2 == 1;
        if (loops[i].circle) {
            shape.kind = SketchShape::Kind::Circle;
            shape.values = {modeling::formatNumber(loops[i].center.x), modeling::formatNumber(loops[i].center.y),
                            modeling::formatNumber(loops[i].radius)};
        } else {
            shape.kind = SketchShape::Kind::Polygon;
            for (const auto& p : loops[i].points) {
                shape.values.push_back(modeling::formatNumber(p.x));
                shape.values.push_back(modeling::formatNumber(p.y));
            }
        }
        shapes.push_back(std::move(shape));
    }
    // Сначала внешние контуры, потом отверстия — так их проще читать в эскизе.
    std::stable_sort(shapes.begin(), shapes.end(), [](const SketchShape& a, const SketchShape& b) { return !a.hole && b.hole; });
    return shapes;
}

// ---- Состав изделия ----

Result<void> AppContext::exportBillOfMaterialsCsv(Document& assemblyDocument, const std::filesystem::path& path) {
    auto bom = assembly().billOfMaterials(assemblyDocument);
    if (!bom) return bom.error();
    const auto cell = [](std::string text) {
        // Кавычки удваиваются, поле берётся в кавычки — так в нём можно писать «;» и переносы.
        std::string out = "\"";
        for (const char c : text) out += c == '"' ? std::string("\"\"") : std::string(1, c);
        return out + "\"";
    };
    const auto decimal = [](double value) {
        std::string text = modeling::formatNumber(std::round(value * 10.0) / 10.0);
        std::replace(text.begin(), text.end(), '.', ',');  // десятичная запятая, как принято в русском Excel
        return text;
    };
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "не удалось открыть файл для записи: " + path.string());
    out << "\xEF\xBB\xBF" << "Поз.;Обозначение;Наименование;Кол.;Материал;Масса 1 шт., г;Масса общая, г\r\n";
    int position = 1;
    for (const auto& line : bom.value())
        out << position++ << ";" << cell(line.designation) << ";" << cell(line.name) << ";" << line.quantity << ";"
            << cell(line.material) << ";" << decimal(line.massGrams) << ";" << decimal(line.massGrams * line.quantity)
            << "\r\n";
    out.flush();
    if (!out) return makeError(ErrorCode::IoError, "не удалось записать файл: " + path.string());
    return {};
}

// ---- Пример ----

namespace {

SketchShape rectangle(const char* x, const char* y, const char* w, const char* h) {
    SketchShape s;
    s.kind = SketchShape::Kind::Rectangle;
    s.values = {x, y, w, h};
    return s;
}

SketchShape circle(const char* x, const char* y, const char* r, bool hole = false) {
    SketchShape s;
    s.kind = SketchShape::Kind::Circle;
    s.values = {x, y, r};
    s.hole = hole;
    return s;
}

}  // namespace

Result<AppContext::DemoModel> AppContext::createDemoModel() {
    DemoModel demo;
    auto& m = modeling();

    // Деталь «Опора»: плита с четырьмя крепёжными отверстиями, бобышка со сквозным отверстием.
    auto support = documents().createDocument(DocumentKind::Part, "Опора");
    if (!support) return support.error();
    demo.support = support.value();
    Document& part = *demo.support;
    for (const auto& [name, value] : {std::pair{"длина", "80"}, std::pair{"ширина", "50"}, std::pair{"толщина", "10"},
                                      std::pair{"диаметр", "24"}, std::pair{"высота_бобышки", "15"},
                                      std::pair{"отступ", "8"}})
        if (auto p = m.addParameter(part, name, value); !p) return p.error();
    if (auto ok = m.setPartProperties(part, "КСМ.003.001", "Сталь 45 ГОСТ 1050-2013", 7.85); !ok) return ok.error();

    auto base = m.addSketch(part, {"Плита", "XY", "0", "",
                                   {rectangle("0", "0", "длина", "ширина"), circle("отступ", "отступ", "3", true),
                                    circle("длина - отступ", "отступ", "3", true),
                                    circle("длина - отступ", "ширина - отступ", "3", true),
                                    circle("отступ", "ширина - отступ", "3", true)}});
    if (!base) return base.error();
    if (auto f = m.addExtrude(part, {base.value(), "толщина", false, false}); !f) return f.error();
    auto boss = m.addSketch(part, {"Бобышка", "", "0", "E1/end", {circle("длина / 2", "ширина / 2", "диаметр / 2 + 6")}});
    if (!boss) return boss.error();
    if (auto f = m.addExtrude(part, {boss.value(), "высота_бобышки", false, false}); !f) return f.error();
    auto bore = m.addSketch(part, {"Отверстие", "", "0", "E2/end", {circle("длина / 2", "ширина / 2", "диаметр / 2")}});
    if (!bore) return bore.error();
    if (auto f = m.addExtrude(part, {bore.value(), "толщина + высота_бобышки", true, true}); !f) return f.error();
    if (auto f = m.addBlend(part, {{{"E1/end", "E2/side/1000"}}, "3", false}); !f) return f.error();
    if (auto f = m.addBlend(part, {{{"E2/end", "E3/side/1000"}}, "1", true}); !f) return f.error();

    // Деталь «Палец».
    auto pin = documents().createDocument(DocumentKind::Part, "Палец");
    if (!pin) return pin.error();
    demo.pin = pin.value();
    if (auto p = m.addParameter(*demo.pin, "диаметр", "23.9"); !p) return p.error();
    if (auto p = m.addParameter(*demo.pin, "длина", "40"); !p) return p.error();
    if (auto ok = m.setPartProperties(*demo.pin, "КСМ.003.002", "Сталь 40Х ГОСТ 4543-2016", 7.82); !ok) return ok.error();
    auto round = m.addSketch(*demo.pin, {"Сечение", "XY", "0", "", {circle("0", "0", "диаметр / 2")}});
    if (!round) return round.error();
    if (auto f = m.addExtrude(*demo.pin, {round.value(), "длина", false, false}); !f) return f.error();
    if (auto f = m.addBlend(*demo.pin, {{{"E1/end", "E1/side/1000"}}, "1", true}); !f) return f.error();

    if (auto ok = rebuildAll(); !ok) return ok.error();
    for (Document* document : {demo.support, demo.pin})
        for (const auto& status : m.status(*document))
            if (status.state == modeling::FeatureState::Failed)
                return makeError(ErrorCode::Internal, "пример: операция не построилась — " + status.diagnostic);

    // Сборка: палец вставлен в отверстие опоры, торец пальца — заподлицо с низом плиты.
    auto assemblyDocument = documents().createDocument(DocumentKind::Assembly, "Опора в сборе");
    if (!assemblyDocument) return assemblyDocument.error();
    demo.assembly = assemblyDocument.value();
    auto first = assembly().addComponent(*demo.assembly, demo.support->id(), geometry::Placement{});
    if (!first) return first.error();
    auto second = assembly().addComponent(*demo.assembly, demo.pin->id(), geometry::Placement::translation(150, 30, 60));
    if (!second) return second.error();
    if (auto mate = assembly().addMate(*demo.assembly, {assembly::MateKind::Concentric,
                                                        {first.value(), "E3/side/1000"},
                                                        {second.value(), "E1/side/1000"}, 0.0, false});
        !mate)
        return mate.error();
    if (auto mate = assembly().addMate(*demo.assembly, {assembly::MateKind::Coincident,
                                                        {first.value(), "E1/start"},
                                                        {second.value(), "E1/start"}, 0.0, true});
        !mate)
        return mate.error();
    auto solved = assembly().solve(*demo.assembly);
    if (!solved) return solved.error();
    if (!solved.value().solved) return makeError(ErrorCode::Internal, "пример: сопряжения сборки не выполнились");

    // Чертёж опоры: три вида по модели.
    auto eskd = standards_->profile("eskd");
    if (!eskd) return eskd.error();
    auto sheetDocument = documents().createDocument(DocumentKind::Drawing, "Опора. Чертёж");
    if (!sheetDocument) return sheetDocument.error();
    demo.drawing = sheetDocument.value();
    Document& sheet = *demo.drawing;
    drawing::SheetSetup setup;
    setup.profileId = "eskd";
    setup.profileVersion = eskd.value().version;
    setup.format = "A4";
    if (auto ok = drawing().setupSheet(sheet, setup); !ok) return ok.error();
    for (const auto& [field, value] : {std::pair{"designation", "КСМ.003.001"}, std::pair{"title", "Опора"},
                                       std::pair{"developer", "Иванов"}, std::pair{"organization", "CosmoCad"},
                                       std::pair{"material", "Сталь 45 ГОСТ 1050-2013"}, std::pair{"scale", "1:1"},
                                       std::pair{"sheetCount", "1"}})
        if (auto ok = drawing().setTitleBlockField(sheet, field, value); !ok) return ok.error();
    if (auto v = addModelView(sheet, part, "front", {85, 235}, 1.0, true); !v) return v.error();
    if (auto v = addModelView(sheet, part, "top", {85, 170}, 1.0, false); !v) return v.error();
    if (auto v = addModelView(sheet, part, "left", {165, 235}, 1.0, true); !v) return v.error();
    auto dimensions = drawing().addLayer(sheet, "Размеры", "solid_thin");
    if (!dimensions) return dimensions.error();
    // Размеры вида сверху: длина, ширина, отверстие; вида спереди: общая высота.
    if (auto d = drawing().addLinearDimension(sheet, {45, 145}, {125, 145}, -10.0, dimensions.value()); !d) return d.error();
    if (auto d = drawing().addLinearDimension(sheet, {45, 145}, {45, 195}, 10.0, dimensions.value()); !d) return d.error();
    if (auto d = drawing().addRadialDimension(sheet, {85, 170}, 12.0, 0.7853981633974483, true, dimensions.value()); !d)
        return d.error();
    if (auto d = drawing().addLinearDimension(sheet, {125, 222.5}, {125, 247.5}, -8.0, dimensions.value()); !d)
        return d.error();
    return demo;
}

}  // namespace cosmocad::app
