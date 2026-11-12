#include "cosmocad/drawing/IDrawingService.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cmath>

#include "cosmocad/document/Transaction.h"

namespace cosmocad::drawing {

using core::ErrorCode;
using core::makeError;
using document::Document;
using document::Transaction;

namespace {

bool finite(Point2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

Result<void> checkLayer(const Document& document, const Uuid& layer) {
    if (layer.isNil() || document.findAs<Layer>(layer)) return {};
    return makeError(ErrorCode::NotFound, "слой не найден");
}

template <class T>
Result<Uuid> addEntity(Document& document, const char* name, std::unique_ptr<T> entity, const Uuid& layer) {
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();
    entity->layerId = layer;
    Transaction tx(document, name);
    auto id = tx.add(std::move(entity));
    if (!id) return id;
    tx.commit();
    return id;
}

}  // namespace

Result<Uuid> DrawingService::setupSheet(Document& document, const SheetSetup& setup) {
    if (document.kind() != document::DocumentKind::Drawing)
        return makeError(ErrorCode::InvalidArgument, "лист можно создать только в чертеже");
    if (!document.objectsOf<Sheet>().empty()) return makeError(ErrorCode::AlreadyExists, "в чертеже уже есть лист");
    if (!(setup.mainLineWidth > 0.0)) return makeError(ErrorCode::InvalidArgument, "толщина линии должна быть больше нуля");

    auto sheet = std::make_unique<Sheet>();
    sheet->format = setup.format;
    sheet->landscape = setup.landscape;
    sheet->mainLineWidth = setup.mainLineWidth;

    Transaction tx(document, "Создать лист");
    auto id = tx.add(std::move(sheet));
    if (!id) return id;
    tx.commit();
    document.profileId = setup.profileId;
    document.profileVersion = setup.profileVersion;
    return id;
}

Result<void> DrawingService::setTitleBlockField(Document& document, const std::string& field,
                                                const std::string& value) {
    auto sheets = document.objectsOf<Sheet>();
    if (sheets.empty()) return makeError(ErrorCode::NotFound, "в чертеже нет листа");
    Transaction tx(document, "Заполнить основную надпись");
    auto modified = tx.modifyAs<Sheet>(sheets.front()->id(), [&](Sheet& sheet) { sheet.titleBlock[field] = value; });
    if (!modified) return modified;
    tx.commit();
    return {};
}

Result<Uuid> DrawingService::addLayer(Document& document, const std::string& name, const std::string& lineTypeId,
                                      const std::string& color) {
    if (name.empty()) return makeError(ErrorCode::InvalidArgument, "имя слоя не задано");
    for (const Layer* existing : document.objectsOf<Layer>())
        if (existing->name == name) return makeError(ErrorCode::AlreadyExists, "слой с таким именем уже есть");
    auto layer = std::make_unique<Layer>();
    layer->name = name;
    layer->lineTypeId = lineTypeId;
    layer->color = color;
    Transaction tx(document, "Создать слой");
    auto id = tx.add(std::move(layer));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<Uuid> DrawingService::addLine(Document& document, Point2 start, Point2 end, const Uuid& layer) {
    if (!finite(start) || !finite(end)) return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    if (geometry::distance(start, end) < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "отрезок нулевой длины");
    auto line = std::make_unique<LineEntity>();
    line->start = start;
    line->end = end;
    return addEntity(document, "Отрезок", std::move(line), layer);
}

Result<Uuid> DrawingService::addCircle(Document& document, Point2 center, double radius, const Uuid& layer) {
    if (!finite(center) || !std::isfinite(radius) || radius < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "радиус должен быть больше нуля");
    auto circle = std::make_unique<CircleEntity>();
    circle->center = center;
    circle->radius = radius;
    return addEntity(document, "Окружность", std::move(circle), layer);
}

Result<Uuid> DrawingService::addArc(Document& document, Point2 center, double radius, double startAngle,
                                    double endAngle, const Uuid& layer) {
    if (!finite(center) || !std::isfinite(radius) || radius < geometry::kLinearTolerance ||
        !std::isfinite(startAngle) || !std::isfinite(endAngle))
        return makeError(ErrorCode::InvalidArgument, "недопустимые параметры дуги");
    auto arc = std::make_unique<ArcEntity>();
    arc->center = center;
    arc->radius = radius;
    arc->startAngle = geometry::normalizeAngle(startAngle);
    arc->endAngle = geometry::normalizeAngle(endAngle);
    return addEntity(document, "Дуга", std::move(arc), layer);
}

Result<Uuid> DrawingService::addText(Document& document, Point2 position, const std::string& text, double height,
                                     const Uuid& layer) {
    if (!finite(position) || !(height > 0.0)) return makeError(ErrorCode::InvalidArgument, "недопустимые параметры текста");
    if (text.empty()) return makeError(ErrorCode::InvalidArgument, "пустой текст");
    auto entity = std::make_unique<TextEntity>();
    entity->position = position;
    entity->text = text;
    entity->height = height;
    return addEntity(document, "Текст", std::move(entity), layer);
}

Result<Uuid> DrawingService::addLinearDimension(Document& document, Point2 p1, Point2 p2, double offset,
                                                const Uuid& layer) {
    if (!finite(p1) || !finite(p2) || !std::isfinite(offset))
        return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    if (geometry::distance(p1, p2) < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "размер нулевой длины");
    auto dimension = std::make_unique<LinearDimension>();
    dimension->p1 = p1;
    dimension->p2 = p2;
    dimension->offset = offset;
    dimension->lineTypeId = "solid_thin";  // размерные линии — тонкие
    return addEntity(document, "Размер", std::move(dimension), layer);
}

Result<std::vector<Uuid>> DrawingService::addRectangle(Document& document, Point2 c1, Point2 c2, const Uuid& layer) {
    if (!finite(c1) || !finite(c2)) return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    if (std::abs(c1.x - c2.x) < geometry::kLinearTolerance || std::abs(c1.y - c2.y) < geometry::kLinearTolerance)
        return makeError(ErrorCode::InvalidArgument, "прямоугольник нулевой ширины или высоты");
    if (auto ok = checkLayer(document, layer); !ok) return ok.error();

    const Point2 corners[4] = {{c1.x, c1.y}, {c2.x, c1.y}, {c2.x, c2.y}, {c1.x, c2.y}};
    std::vector<Uuid> ids;
    Transaction tx(document, "Прямоугольник");
    for (int i = 0; i < 4; ++i) {
        auto line = std::make_unique<LineEntity>();
        line->start = corners[i];
        line->end = corners[(i + 1) % 4];
        line->layerId = layer;
        auto id = tx.add(std::move(line));
        if (!id) return id.error();
        ids.push_back(id.value());
    }
    tx.commit();
    return ids;
}

namespace {

Result<void> checkTransform(const geometry::Transform2& t) {
    if (!std::isfinite(t.a) || !std::isfinite(t.b) || !std::isfinite(t.tx) || !std::isfinite(t.ty))
        return makeError(ErrorCode::InvalidArgument, "недопустимое преобразование");
    if (t.scale() < 1e-9) return makeError(ErrorCode::InvalidArgument, "масштаб должен быть больше нуля");
    return {};
}

}  // namespace

Result<void> DrawingService::moveEntities(Document& document, const std::vector<Uuid>& ids, Point2 delta) {
    if (!finite(delta)) return makeError(ErrorCode::InvalidArgument, "недопустимое смещение");
    return transformEntities(document, ids, geometry::Transform2::translation(delta), "Переместить");
}

Result<void> DrawingService::transformEntities(Document& document, const std::vector<Uuid>& ids,
                                               const geometry::Transform2& transform, const std::string& name) {
    if (auto ok = checkTransform(transform); !ok) return ok;
    Transaction tx(document, name);
    for (const Uuid& id : ids) {
        if (!document.findAs<Entity>(id)) return makeError(ErrorCode::NotFound, "элемент не найден");
        auto changed = tx.modify(id, [&](document::Object& object) { static_cast<Entity&>(object).transform(transform); });
        if (!changed) return changed;  // транзакция откатится целиком
    }
    tx.commit();
    return {};
}

Result<std::vector<Uuid>> DrawingService::copyEntities(Document& document, const std::vector<Uuid>& ids,
                                                       const geometry::Transform2& transform) {
    if (auto ok = checkTransform(transform); !ok) return ok.error();
    std::vector<Uuid> copies;
    Transaction tx(document, "Копировать");
    for (const Uuid& id : ids) {
        const Entity* source = document.findAs<Entity>(id);
        if (!source) return makeError(ErrorCode::NotFound, "элемент не найден");
        auto copy = source->duplicate();
        static_cast<Entity&>(*copy).transform(transform);
        auto added = tx.add(std::move(copy));
        if (!added) return added.error();
        copies.push_back(added.value());
    }
    tx.commit();
    return copies;
}

Result<void> DrawingService::setEntitiesLayer(Document& document, const std::vector<Uuid>& ids, const Uuid& layer) {
    if (auto ok = checkLayer(document, layer); !ok) return ok;
    Transaction tx(document, "Сменить слой");
    for (const Uuid& id : ids) {
        auto changed = tx.modifyAs<Entity>(id, [&](Entity& entity) { entity.layerId = layer; });
        if (!changed) return changed;
    }
    tx.commit();
    return {};
}

Result<void> DrawingService::removeObjects(Document& document, const std::vector<Uuid>& ids) {
    Transaction tx(document, "Удалить");
    for (const Uuid& id : ids)
        if (auto removed = tx.remove(id); !removed) return removed;
    tx.commit();
    return {};
}

}  // namespace cosmocad::drawing

namespace cosmocad::drawing {

Result<void> DrawingService::setLayerState(Document& document, const Uuid& layer, bool visible, bool locked) {
    Transaction tx(document, "Изменить слой");
    auto changed = tx.modifyAs<Layer>(layer, [&](Layer& l) {
        l.visible = visible;
        l.locked = locked;
    });
    if (!changed) return changed;
    tx.commit();
    return {};
}

Result<void> DrawingService::updateSheet(Document& document, const std::string& format, bool landscape,
                                         double mainLineWidth, const std::map<std::string, std::string>& titleBlock) {
    auto sheets = document.objectsOf<Sheet>();
    if (sheets.empty()) return makeError(ErrorCode::NotFound, "в чертеже нет листа");
    if (format.empty()) return makeError(ErrorCode::InvalidArgument, "формат листа не задан");
    if (!(mainLineWidth > 0.0) || !std::isfinite(mainLineWidth))
        return makeError(ErrorCode::InvalidArgument, "толщина линии должна быть больше нуля");
    Transaction tx(document, "Изменить лист");
    auto changed = tx.modifyAs<Sheet>(sheets.front()->id(), [&](Sheet& sheet) {
        sheet.format = format;
        sheet.landscape = landscape;
        sheet.mainLineWidth = mainLineWidth;
        sheet.titleBlock = titleBlock;
    });
    if (!changed) return changed;
    tx.commit();
    return {};
}

}  // namespace cosmocad::drawing
