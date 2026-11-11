#include "cosmocad/schematic/Schematic.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <numeric>
#include <set>

#include <nlohmann/json.hpp>

#include "cosmocad/document/Transaction.h"

namespace cosmocad::schematic {

using core::ErrorCode;
using core::makeError;
using document::Document;
using document::Transaction;
using nlohmann::json;

namespace {

const double kTolerance = 1e-6;

json pointToJson(Point2 p) { return json::array({p.x, p.y}); }
Point2 pointFromJson(const json& j) { return Point2{j.at(0).get<double>(), j.at(1).get<double>()}; }

json primitiveToJson(const Primitive& p) {
    switch (p.kind) {
        case Primitive::Kind::Circle: return {{"circle", {p.a.x, p.a.y, p.radius}}};
        case Primitive::Kind::Arc: return {{"arc", {p.a.x, p.a.y, p.radius, p.startAngle, p.endAngle}}};
        case Primitive::Kind::Text: return {{"text", {p.a.x, p.a.y, p.textHeight, p.text}}};
        default: return {{"line", {p.a.x, p.a.y, p.b.x, p.b.y}}};
    }
}

// Читает один графический элемент; прямоугольник превращается в четыре отрезка.
void appendPrimitive(const json& item, std::vector<Primitive>& out) {
    const auto line = [&](double x1, double y1, double x2, double y2) {
        Primitive p;
        p.kind = Primitive::Kind::Line;
        p.a = {x1, y1};
        p.b = {x2, y2};
        out.push_back(p);
    };
    if (item.contains("line")) {
        const auto& v = item.at("line");
        line(v.at(0), v.at(1), v.at(2), v.at(3));
    } else if (item.contains("rect")) {
        const auto& v = item.at("rect");
        const double x1 = v.at(0), y1 = v.at(1), x2 = v.at(2), y2 = v.at(3);
        line(x1, y1, x2, y1);
        line(x2, y1, x2, y2);
        line(x2, y2, x1, y2);
        line(x1, y2, x1, y1);
    } else if (item.contains("circle")) {
        const auto& v = item.at("circle");
        Primitive p;
        p.kind = Primitive::Kind::Circle;
        p.a = {v.at(0), v.at(1)};
        p.radius = v.at(2);
        out.push_back(p);
    } else if (item.contains("arc")) {
        const auto& v = item.at("arc");
        Primitive p;
        p.kind = Primitive::Kind::Arc;
        p.a = {v.at(0), v.at(1)};
        p.radius = v.at(2);
        p.startAngle = v.at(3);
        p.endAngle = v.at(4);
        out.push_back(p);
    } else if (item.contains("text")) {
        // Надпись внутри обозначения (буква в приборе): x, y середины нижнего края, высота, текст.
        const auto& v = item.at("text");
        Primitive p;
        p.kind = Primitive::Kind::Text;
        p.a = {v.at(0), v.at(1)};
        p.textHeight = v.at(2);
        p.text = v.at(3).get<std::string>();
        out.push_back(p);
    } else {
        throw json::other_error::create(0, "неизвестный графический элемент обозначения", &item);
    }
}

bool onSegment(Point2 p, Point2 a, Point2 b) { return geometry::distanceToSegment(p, a, b) <= kTolerance; }

// «R12» → («R», 12). Обозначение без номера получает номер 0.
std::pair<std::string, int> splitDesignator(const std::string& text) {
    std::size_t i = text.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]))) --i;
    return {text.substr(0, i), i < text.size() ? std::atoi(text.c_str() + i) : 0};
}

bool designatorLess(const std::string& a, const std::string& b) { return splitDesignator(a) < splitDesignator(b); }

Result<void> requireDrawing(const Document& document) {
    if (document.kind() != document::DocumentKind::Drawing)
        return makeError(ErrorCode::InvalidArgument, "схема размещается на чертеже");
    return {};
}

// Сеть соединений схемы: что с чем электрически связано.
struct Connectivity {
    struct Node {
        Point2 at;
        enum class Kind { WireVertex, Pin, Label } kind;
        const drawing::Entity* owner;
        std::size_t index;  // номер вершины проводника или вывода
        bool wireEnd;
    };
    std::vector<Node> nodes;
    std::vector<std::size_t> parent;

    std::size_t find(std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    }
    void unite(std::size_t a, std::size_t b) { parent[find(a)] = find(b); }

    explicit Connectivity(const Document& document) {
        const auto wires = document.objectsOf<Wire>();
        for (const auto* wire : wires)
            for (std::size_t i = 0; i < wire->points.size(); ++i)
                nodes.push_back({wire->points[i], Node::Kind::WireVertex, wire, i, i == 0 || i + 1 == wire->points.size()});
        for (const auto* element : document.objectsOf<Element>())
            for (std::size_t i = 0; i < element->pins.size(); ++i)
                nodes.push_back({element->pinPosition(i), Node::Kind::Pin, element, i, false});
        for (const auto* label : document.objectsOf<Label>())
            nodes.push_back({label->position, Node::Kind::Label, label, 0, false});
        parent.resize(nodes.size());
        std::iota(parent.begin(), parent.end(), std::size_t{0});

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            // Вершины одного проводника связаны между собой.
            if (i > 0 && nodes[i].kind == Node::Kind::WireVertex && nodes[i - 1].owner == nodes[i].owner) unite(i, i - 1);
            // Совпадающие точки связаны.
            for (std::size_t j = 0; j < i; ++j)
                if (geometry::almostEqual(nodes[i].at, nodes[j].at, kTolerance)) unite(i, j);
        }
        // Вывод, метка или конец проводника, лежащие на другом проводнике, связаны с ним.
        std::size_t base = 0;
        for (const auto* wire : wires) {
            for (std::size_t s = 0; s + 1 < wire->points.size(); ++s)
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    const Node& node = nodes[i];
                    if (node.owner == wire) continue;
                    if (node.kind == Node::Kind::WireVertex && !node.wireEnd) continue;
                    if (onSegment(node.at, wire->points[s], wire->points[s + 1])) unite(i, base + s);
                }
            base += wire->points.size();
        }
    }
};

}  // namespace

// ---- Набор обозначений ----

Result<SymbolLibrary> SymbolLibrary::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return makeError(ErrorCode::IoError, "не удалось открыть набор обозначений: " + file.string());
    try {
        const json root = json::parse(in);
        SymbolLibrary library;
        library.id = root.value("id", std::string());
        library.version = root.at("version").get<std::string>();
        std::set<std::string> ids;
        for (const auto& item : root.at("symbols")) {
            SymbolDef symbol;
            symbol.id = item.at("id").get<std::string>();
            symbol.name = item.at("name").get<std::string>();
            symbol.prefix = item.at("prefix").get<std::string>();
            symbol.standard = item.value("standard", std::string());
            symbol.source = item.value("source", std::string());
            for (const auto& pin : item.at("pins")) symbol.pins.push_back({pin.at("name").get<std::string>(), pointFromJson(pin.at("at"))});
            for (const auto& graphic : item.at("graphics")) appendPrimitive(graphic, symbol.graphics);
            if (symbol.id.empty() || symbol.prefix.empty() || symbol.pins.empty() || symbol.graphics.empty())
                return makeError(ErrorCode::ParseError, "набор обозначений: у обозначения «" + symbol.id +
                                                            "» нет буквы, выводов или графики");
            if (!ids.insert(symbol.id).second)
                return makeError(ErrorCode::ParseError, "набор обозначений: обозначение «" + symbol.id + "» повторяется");
            library.symbols.push_back(std::move(symbol));
        }
        return library;
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("набор обозначений: ") + e.what());
    }
}

const SymbolDef* SymbolLibrary::find(const std::string& symbolId) const {
    for (const auto& symbol : symbols)
        if (symbol.id == symbolId) return &symbol;
    return nullptr;
}

// ---- Элемент ----

Point2 Element::toSheet(Point2 local) const {
    const double angle = rotation * std::numbers::pi / 180.0;
    // Повороты кратны 90°, поэтому синус и косинус округляются до точных −1, 0, 1.
    const double c = std::round(std::cos(angle)), s = std::round(std::sin(angle));
    return {position.x + local.x * c - local.y * s, position.y + local.x * s + local.y * c};
}

BBox2 Element::bodyBounds() const {
    BBox2 box;
    for (const auto& g : graphics) {
        if (g.kind == Primitive::Kind::Line) {
            box.expand(toSheet(g.a));
            box.expand(toSheet(g.b));
        } else if (g.kind == Primitive::Kind::Text) {
            continue;  // буква внутри обозначения габарит не увеличивает
        } else {
            const Point2 c = toSheet(g.a);
            box.expand(Point2{c.x - g.radius, c.y - g.radius});
            box.expand(Point2{c.x + g.radius, c.y + g.radius});
        }
    }
    for (std::size_t i = 0; i < pins.size(); ++i) box.expand(pinPosition(i));
    return box;
}

namespace {

struct TextSpot {
    std::string text;
    Point2 at;  // середина нижнего края строки
};

// Оценка ширины строки с запасом: надпись не должна задевать сам элемент.
double textWidth(const std::string& text, double height) { return drawing::characterCount(text) * height * 0.75; }

}  // namespace

// Где стоят надписи. У горизонтального элемента обозначение — над ним, номинал — под ним;
// у вертикального обе надписи справа, чтобы не лечь на проводники, идущие вверх и вниз.
static std::vector<TextSpot> textSpots(const Element& e, const BBox2& box) {
    std::vector<TextSpot> out;
    const bool vertical = e.rotation % 180 != 0;
    if (!vertical) {
        const double middle = (box.min.x + box.max.x) / 2.0;
        if (!e.designator.empty()) out.push_back({e.designator, {middle, box.max.y + 1.5}});
        if (!e.value.empty()) out.push_back({e.value, {middle, box.min.y - 1.5 - e.textHeight}});
    } else {
        const double middle = (box.min.y + box.max.y) / 2.0;
        const double left = box.max.x + 2.0;
        if (!e.designator.empty()) out.push_back({e.designator, {left + textWidth(e.designator, e.textHeight) / 2.0, middle + 0.75}});
        if (!e.value.empty()) out.push_back({e.value, {left + textWidth(e.value, e.textHeight) / 2.0, middle - 0.75 - e.textHeight}});
    }
    return out;
}

std::vector<Primitive> Element::primitives() const {
    std::vector<Primitive> out;
    const double turn = rotation * std::numbers::pi / 180.0;
    for (Primitive g : graphics) {
        if (g.kind == Primitive::Kind::Text) {
            // Буква не поворачивается вместе с элементом: её середина остаётся в той же точке обозначения.
            const Point2 middle = toSheet({g.a.x, g.a.y + g.textHeight / 2.0});
            g.a = {middle.x, middle.y - g.textHeight / 2.0};
            out.push_back(g);
            continue;
        }
        g.a = toSheet(g.a);
        if (g.kind == Primitive::Kind::Line) g.b = toSheet(g.b);
        if (g.kind == Primitive::Kind::Arc) {
            g.startAngle += turn;
            g.endAngle += turn;
        }
        out.push_back(g);
    }
    for (const auto& spot : textSpots(*this, bodyBounds())) {
        Primitive t;
        t.kind = Primitive::Kind::Text;
        t.a = spot.at;
        t.text = spot.text;
        t.textHeight = textHeight;
        out.push_back(t);
    }
    return out;
}

BBox2 Element::bounds() const {
    const BBox2 body = bodyBounds();
    BBox2 box = body;
    for (const auto& spot : textSpots(*this, body)) {
        const double half = textWidth(spot.text, textHeight) / 2.0;
        box.expand(Point2{spot.at.x - half, spot.at.y});
        box.expand(Point2{spot.at.x + half, spot.at.y + textHeight});
    }
    return box;
}

void Element::transform(const geometry::Transform2& t) {
    // Элемент переносится и поворачивается на углы, кратные 90°; отражение и масштаб не применяются.
    position = t.apply(position);
    const int quarter = static_cast<int>(std::lround(t.angle() / (std::numbers::pi / 2.0)));
    rotation = ((rotation + quarter * 90) % 360 + 360) % 360;
}

std::vector<Point2> Element::snapPoints() const {
    std::vector<Point2> out;
    for (std::size_t i = 0; i < pins.size(); ++i) out.push_back(pinPosition(i));
    return out;
}

void Element::writeJson(json& j) const {
    writeEntityJson(j);
    j["symbol"] = symbolId;
    j["symbolName"] = symbolName;
    j["libraryVersion"] = libraryVersion;
    j["prefix"] = prefix;
    j["designator"] = designator;
    j["value"] = value;
    j["position"] = pointToJson(position);
    j["rotation"] = rotation;
    j["textHeight"] = textHeight;
    json pinList = json::array();
    for (const auto& pin : pins) pinList.push_back({{"name", pin.name}, {"at", pointToJson(pin.at)}});
    j["pins"] = std::move(pinList);
    json graphicList = json::array();
    for (const auto& g : graphics) graphicList.push_back(primitiveToJson(g));
    j["graphics"] = std::move(graphicList);
}

Result<void> Element::readJson(const json& j) {
    try {
        if (auto r = readEntityJson(j); !r) return r;
        symbolId = j.at("symbol").get<std::string>();
        symbolName = j.value("symbolName", std::string());
        libraryVersion = j.value("libraryVersion", std::string());
        prefix = j.at("prefix").get<std::string>();
        designator = j.at("designator").get<std::string>();
        value = j.value("value", std::string());
        position = pointFromJson(j.at("position"));
        rotation = j.value("rotation", 0);
        textHeight = j.value("textHeight", 3.5);
        pins.clear();
        for (const auto& pin : j.at("pins")) pins.push_back({pin.at("name").get<std::string>(), pointFromJson(pin.at("at"))});
        graphics.clear();
        for (const auto& g : j.at("graphics")) appendPrimitive(g, graphics);
        if (rotation % 90 != 0) return makeError(ErrorCode::ParseError, "элемент схемы: поворот должен быть кратен 90°");
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("элемент схемы: ") + e.what());
    }
}

// ---- Проводник ----

BBox2 Wire::bounds() const {
    BBox2 box;
    for (const auto& p : points) box.expand(p);
    return box;
}

void Wire::transform(const geometry::Transform2& t) {
    for (auto& p : points) p = t.apply(p);
    for (auto& p : junctions) p = t.apply(p);
}

std::vector<Primitive> Wire::primitives() const {
    std::vector<Primitive> out;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        Primitive p;
        p.kind = Primitive::Kind::Line;
        p.a = points[i];
        p.b = points[i + 1];
        out.push_back(p);
    }
    for (const auto& at : junctions) {
        Primitive p;
        p.kind = Primitive::Kind::Dot;
        p.a = at;
        p.radius = 0.8;
        out.push_back(p);
    }
    return out;
}

void Wire::writeJson(json& j) const {
    writeEntityJson(j);
    json list = json::array();
    for (const auto& p : points) list.push_back(pointToJson(p));
    j["points"] = std::move(list);
    json dots = json::array();
    for (const auto& p : junctions) dots.push_back(pointToJson(p));
    j["junctions"] = std::move(dots);
}

Result<void> Wire::readJson(const json& j) {
    try {
        if (auto r = readEntityJson(j); !r) return r;
        points.clear();
        junctions.clear();
        for (const auto& p : j.at("points")) points.push_back(pointFromJson(p));
        if (j.contains("junctions"))
            for (const auto& p : j.at("junctions")) junctions.push_back(pointFromJson(p));
        if (points.size() < 2) return makeError(ErrorCode::ParseError, "проводник: меньше двух точек");
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("проводник: ") + e.what());
    }
}

// ---- Метка цепи ----

BBox2 Label::bounds() const {
    BBox2 box;
    const double half = drawing::characterCount(text) * textHeight * 0.6 / 2.0;
    box.expand(Point2{position.x - half, position.y});
    box.expand(Point2{position.x + half, position.y + 1.0 + textHeight});
    return box;
}

void Label::transform(const geometry::Transform2& t) { position = t.apply(position); }

std::vector<Primitive> Label::primitives() const {
    Primitive t;
    t.kind = Primitive::Kind::Text;
    t.a = {position.x, position.y + 1.5};
    t.text = text;
    t.textHeight = textHeight;
    return {t};
}

void Label::writeJson(json& j) const {
    writeEntityJson(j);
    j["position"] = pointToJson(position);
    j["text"] = text;
    j["textHeight"] = textHeight;
}

Result<void> Label::readJson(const json& j) {
    try {
        if (auto r = readEntityJson(j); !r) return r;
        position = pointFromJson(j.at("position"));
        text = j.at("text").get<std::string>();
        textHeight = j.value("textHeight", 3.5);
        return {};
    } catch (const json::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("метка цепи: ") + e.what());
    }
}

void registerTypes(document::ObjectFactory& factory) {
    factory.registerType<Element>();
    factory.registerType<Wire>();
    factory.registerType<Label>();
}

// ---- Сервис ----

namespace {

// Точки соединения: конец проводника, лежащий на другом проводнике, и место, где сходятся три конца и больше.
std::map<Uuid, std::vector<Point2>> computeJunctions(const Document& document) {
    std::map<Uuid, std::vector<Point2>> out;
    const auto wires = document.objectsOf<Wire>();
    for (const auto* wire : wires) {
        std::vector<Point2>& dots = out[wire->id()];
        for (const std::size_t index : {std::size_t{0}, wire->points.size() - 1}) {
            const Point2 end = wire->points[index];
            int endsHere = 1;
            bool onOther = false;
            for (const auto* other : wires) {
                if (other == wire) continue;
                for (std::size_t i = 0; i < other->points.size(); ++i) {
                    const bool isEnd = i == 0 || i + 1 == other->points.size();
                    if (geometry::almostEqual(other->points[i], end, kTolerance)) {
                        if (isEnd)
                            ++endsHere;
                        else
                            onOther = true;
                    }
                }
                for (std::size_t s = 0; s + 1 < other->points.size(); ++s)
                    if (onSegment(end, other->points[s], other->points[s + 1]) &&
                        !geometry::almostEqual(end, other->points[s], kTolerance) &&
                        !geometry::almostEqual(end, other->points[s + 1], kTolerance))
                        onOther = true;
            }
            if (onOther || endsHere >= 3) dots.push_back(end);
        }
    }
    return out;
}

Result<void> applyJunctions(Document& document, Transaction& tx) {
    for (const auto& [id, dots] : computeJunctions(document)) {
        const auto* wire = document.findAs<Wire>(id);
        if (!wire) continue;
        bool same = wire->junctions.size() == dots.size();
        for (std::size_t i = 0; same && i < dots.size(); ++i) same = geometry::almostEqual(wire->junctions[i], dots[i], kTolerance);
        if (same) continue;
        if (auto ok = tx.modifyAs<Wire>(id, [&](Wire& w) { w.junctions = dots; }); !ok) return ok;
    }
    return {};
}

}  // namespace

Result<Uuid> SchematicService::addElement(Document& document, const std::string& symbolId, Point2 position,
                                          int rotationDegrees, const std::string& value) {
    if (auto ok = requireDrawing(document); !ok) return ok.error();
    const SymbolDef* symbol = library_.find(symbolId);
    if (!symbol) return makeError(ErrorCode::NotFound, "в наборе обозначений нет «" + symbolId + "»");
    if (rotationDegrees % 90 != 0) return makeError(ErrorCode::InvalidArgument, "элемент поворачивается на угол, кратный 90°");
    if (!std::isfinite(position.x) || !std::isfinite(position.y))
        return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
    int number = 0;
    for (const auto* e : document.objectsOf<Element>()) {
        const auto [prefix, n] = splitDesignator(e->designator);
        if (prefix == symbol->prefix) number = std::max(number, n);
    }
    auto element = std::make_unique<Element>();
    element->name = symbol->name;
    element->symbolId = symbol->id;
    element->symbolName = symbol->name;
    element->libraryVersion = library_.version;
    element->prefix = symbol->prefix;
    element->designator = symbol->prefix + std::to_string(number + 1);
    element->value = value;
    element->position = position;
    element->rotation = ((rotationDegrees % 360) + 360) % 360;
    element->pins = symbol->pins;
    element->graphics = symbol->graphics;
    element->lineTypeId = "solid_thin";
    Transaction tx(document, "Элемент схемы");
    auto id = tx.add(std::move(element));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<Uuid> SchematicService::addWire(Document& document, const std::vector<Point2>& points) {
    if (auto ok = requireDrawing(document); !ok) return ok.error();
    if (points.size() < 2) return makeError(ErrorCode::InvalidArgument, "у проводника должно быть не меньше двух точек");
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y))
            return makeError(ErrorCode::InvalidArgument, "недопустимые координаты");
        if (i > 0 && geometry::almostEqual(points[i], points[i - 1], kTolerance))
            return makeError(ErrorCode::InvalidArgument, "у проводника есть участок нулевой длины");
    }
    auto wire = std::make_unique<Wire>();
    wire->name = "Проводник";
    wire->points = points;
    wire->lineTypeId = "solid_thin";
    Transaction tx(document, "Проводник");
    auto id = tx.add(std::move(wire));
    if (!id) return id;
    if (auto ok = applyJunctions(document, tx); !ok) return ok.error();
    tx.commit();
    return id;
}

Result<Uuid> SchematicService::addLabel(Document& document, Point2 position, const std::string& text) {
    if (auto ok = requireDrawing(document); !ok) return ok.error();
    if (text.empty()) return makeError(ErrorCode::InvalidArgument, "имя цепи пусто");
    auto label = std::make_unique<Label>();
    label->name = "Метка цепи";
    label->position = position;
    label->text = text;
    label->lineTypeId = "solid_thin";
    Transaction tx(document, "Метка цепи");
    auto id = tx.add(std::move(label));
    if (!id) return id;
    tx.commit();
    return id;
}

Result<void> SchematicService::setValue(Document& document, const Uuid& elementId, const std::string& value) {
    Transaction tx(document, "Номинал элемента");
    if (auto ok = tx.modifyAs<Element>(elementId, [&](Element& e) { e.value = value; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> SchematicService::setDesignator(Document& document, const Uuid& elementId, const std::string& designator) {
    if (designator.empty()) return makeError(ErrorCode::InvalidArgument, "позиционное обозначение пусто");
    for (const auto* e : document.objectsOf<Element>())
        if (e->id() != elementId && e->designator == designator)
            return makeError(ErrorCode::AlreadyExists, "обозначение «" + designator + "» уже занято");
    Transaction tx(document, "Позиционное обозначение");
    if (auto ok = tx.modifyAs<Element>(elementId, [&](Element& e) { e.designator = designator; }); !ok) return ok;
    tx.commit();
    return {};
}

Result<void> SchematicService::moveElements(Document& document, const std::vector<Uuid>& elementIds, Point2 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y)) return makeError(ErrorCode::InvalidArgument, "недопустимое смещение");
    // Где сейчас выводы переносимых элементов: концы проводников в этих точках поедут следом.
    std::vector<Point2> pinPoints;
    for (const auto& id : elementIds) {
        const auto* element = document.findAs<Element>(id);
        if (!element) return makeError(ErrorCode::NotFound, "элемент схемы не найден");
        for (std::size_t i = 0; i < element->pins.size(); ++i) pinPoints.push_back(element->pinPosition(i));
    }
    const auto attached = [&](Point2 p) {
        return std::any_of(pinPoints.begin(), pinPoints.end(), [&](Point2 pin) { return geometry::almostEqual(pin, p, kTolerance); });
    };
    Transaction tx(document, "Перенос элементов схемы");
    for (const auto& id : elementIds)
        if (auto ok = tx.modifyAs<Element>(id, [&](Element& e) { e.position = e.position + delta; }); !ok) return ok;
    for (const auto* wire : document.objectsOf<Wire>()) {
        const bool first = attached(wire->points.front()), last = attached(wire->points.back());
        if (!first && !last) continue;
        // Конец едет за выводом. Если сдвиг идёт вдоль проводника, конец просто перемещается;
        // иначе старый конец остаётся изломом, чтобы ответвления на проводнике не оторвались.
        const auto moveEnd = [&](std::vector<Point2>& points, bool front) {
            const Point2 oldEnd = front ? points.front() : points.back();
            const Point2 next = front ? points[1] : points[points.size() - 2];
            const Point2 newEnd = oldEnd + delta;
            const bool along = std::abs(geometry::cross(delta, next - oldEnd)) < kTolerance &&
                               geometry::distance(newEnd, next) > kTolerance;
            std::vector<Point2> lead{newEnd};
            if (!along) {
                if (std::abs(delta.x) > kTolerance && std::abs(delta.y) > kTolerance) lead.push_back({newEnd.x, oldEnd.y});
                lead.push_back(oldEnd);
            }
            if (front) {
                points.erase(points.begin());
                points.insert(points.begin(), lead.begin(), lead.end());
            } else {
                points.pop_back();
                points.insert(points.end(), lead.rbegin(), lead.rend());
            }
        };
        auto ok = tx.modifyAs<Wire>(wire->id(), [&](Wire& w) {
            if (first) moveEnd(w.points, true);
            if (last) moveEnd(w.points, false);
        });
        if (!ok) return ok;
    }
    if (auto ok = applyJunctions(document, tx); !ok) return ok;
    tx.commit();
    return {};
}

Result<int> SchematicService::renumber(Document& document) {
    auto elements = document.objectsOf<Element>();
    std::stable_sort(elements.begin(), elements.end(), [](const Element* a, const Element* b) {
        if (std::abs(a->position.x - b->position.x) > kTolerance) return a->position.x < b->position.x;
        return a->position.y > b->position.y;  // в столбце — сверху вниз
    });
    std::map<std::string, int> counters;
    std::vector<std::pair<Uuid, std::string>> changes;
    for (const auto* element : elements) {
        const std::string designator = element->prefix + std::to_string(++counters[element->prefix]);
        if (designator != element->designator) changes.emplace_back(element->id(), designator);
    }
    if (changes.empty()) return 0;
    Transaction tx(document, "Перенумерация элементов");
    for (const auto& [id, designator] : changes)
        if (auto ok = tx.modifyAs<Element>(id, [&](Element& e) { e.designator = designator; }); !ok) return ok.error();
    tx.commit();
    return static_cast<int>(changes.size());
}

Result<int> SchematicService::numberNets(Document& document) {
    if (auto ok = requireDrawing(document); !ok) return ok.error();
    const auto all = nets(document);
    std::set<std::string> used;
    for (const auto* label : document.objectsOf<Label>()) used.insert(label->text);
    int next = 1, added = 0;
    Transaction tx(document, "Номера цепей");
    for (const auto& net : all) {
        if (!net.name.empty() || net.wires.empty()) continue;
        // Номер ставится над серединой самого длинного горизонтального участка цепи;
        // если горизонтальных нет — у середины самого длинного участка.
        Point2 best;
        double bestLength = -1.0;
        bool bestHorizontal = false;
        for (const auto& wireId : net.wires) {
            const auto* wire = document.findAs<Wire>(wireId);
            if (!wire) continue;
            for (std::size_t i = 0; i + 1 < wire->points.size(); ++i) {
                const Point2 a = wire->points[i], b = wire->points[i + 1];
                const double length = geometry::distance(a, b);
                const bool horizontal = std::abs(a.y - b.y) < 1e-9;
                if ((horizontal && !bestHorizontal) || (horizontal == bestHorizontal && length > bestLength + 1e-9)) {
                    best = geometry::midpoint(a, b);
                    bestLength = length;
                    bestHorizontal = horizontal;
                }
            }
        }
        if (bestLength < 0.0) continue;
        while (used.count(std::to_string(next))) ++next;
        const std::string text = std::to_string(next);
        used.insert(text);
        auto label = std::make_unique<Label>();
        label->name = "Номер цепи";
        // Число пишется от точки вправо; сдвигаем точку влево, чтобы оно встало по середине участка.
        label->position = bestHorizontal ? Point2{best.x - 0.9 * static_cast<double>(text.size()), best.y} : best;
        label->text = text;
        label->lineTypeId = "solid_thin";
        if (auto id = tx.add(std::move(label)); !id) return id.error();
        ++added;
    }
    if (added == 0) {
        tx.rollback();
        return 0;
    }
    tx.commit();
    return added;
}

Result<void> SchematicService::refreshJunctions(Document& document) {
    Transaction tx(document, "Точки соединения");
    if (auto ok = applyJunctions(document, tx); !ok) return ok;
    tx.commit();
    return {};
}

std::vector<Net> SchematicService::nets(const Document& document) const {
    Connectivity c(document);
    std::map<std::size_t, Net> groups;
    std::map<std::size_t, std::set<std::string>> names;
    for (std::size_t i = 0; i < c.nodes.size(); ++i) {
        const auto& node = c.nodes[i];
        Net& net = groups[c.find(i)];
        if (node.kind == Connectivity::Node::Kind::Pin) {
            const auto* element = static_cast<const Element*>(node.owner);
            net.pins.push_back({element->id(), element->designator, element->pins[node.index].name});
        } else if (node.kind == Connectivity::Node::Kind::WireVertex) {
            if (std::find(net.wires.begin(), net.wires.end(), node.owner->id()) == net.wires.end())
                net.wires.push_back(node.owner->id());
        } else {
            names[c.find(i)].insert(static_cast<const Label*>(node.owner)->text);
        }
    }
    std::vector<Net> out;
    for (auto& [root, net] : groups) {
        if (net.pins.empty() && net.wires.empty()) continue;  // одинокая метка — не цепь
        std::sort(net.pins.begin(), net.pins.end(), [](const NetPin& a, const NetPin& b) {
            if (a.designator != b.designator) return designatorLess(a.designator, b.designator);
            return a.pin < b.pin;
        });
        if (const auto it = names.find(root); it != names.end()) {
            for (const auto& name : it->second) net.name += (net.name.empty() ? "" : " / ") + name;
        }
        out.push_back(std::move(net));
    }
    // Порядок цепей — по первому выводу; цепи без выводов — в конце.
    std::stable_sort(out.begin(), out.end(), [](const Net& a, const Net& b) {
        if (a.pins.empty() != b.pins.empty()) return b.pins.empty();
        if (a.pins.empty()) return false;
        if (a.pins[0].designator != b.pins[0].designator) return designatorLess(a.pins[0].designator, b.pins[0].designator);
        return a.pins[0].pin < b.pins[0].pin;
    });
    for (std::size_t i = 0; i < out.size(); ++i) out[i].number = static_cast<int>(i + 1);
    return out;
}

std::vector<ListRow> SchematicService::elementList(const Document& document) const {
    auto elements = document.objectsOf<Element>();
    std::stable_sort(elements.begin(), elements.end(),
                     [](const Element* a, const Element* b) { return designatorLess(a->designator, b->designator); });
    std::vector<ListRow> rows;
    std::size_t i = 0;
    while (i < elements.size()) {
        // Подряд идущие одинаковые элементы записываются одной строкой.
        std::size_t j = i + 1;
        while (j < elements.size() && elements[j]->prefix == elements[i]->prefix &&
               elements[j]->symbolName == elements[i]->symbolName && elements[j]->value == elements[i]->value &&
               splitDesignator(elements[j]->designator).second == splitDesignator(elements[j - 1]->designator).second + 1)
            ++j;
        ListRow row;
        row.count = static_cast<int>(j - i);
        row.name = elements[i]->symbolName + (elements[i]->value.empty() ? "" : " " + elements[i]->value);
        if (row.count == 1)
            row.designators = elements[i]->designator;
        else if (row.count == 2)
            row.designators = elements[i]->designator + ", " + elements[j - 1]->designator;
        else
            row.designators = elements[i]->designator + " ... " + elements[j - 1]->designator;  // запись по ГОСТ 2.701-2008, п. 5.7
        rows.push_back(std::move(row));
        i = j;
    }
    return rows;
}

std::vector<Issue> SchematicService::check(const Document& document) const {
    std::vector<Issue> issues;
    Connectivity c(document);
    // Сколько чего в каждой группе связанных точек.
    struct Group {
        int pins = 0, wires = 0, labels = 0;
        std::set<std::string> names;
    };
    std::map<std::size_t, Group> groups;
    for (std::size_t i = 0; i < c.nodes.size(); ++i) {
        Group& g = groups[c.find(i)];
        switch (c.nodes[i].kind) {
            case Connectivity::Node::Kind::Pin: ++g.pins; break;
            case Connectivity::Node::Kind::WireVertex: ++g.wires; break;
            case Connectivity::Node::Kind::Label:
                ++g.labels;
                g.names.insert(static_cast<const Label*>(c.nodes[i].owner)->text);
                break;
        }
    }
    for (std::size_t i = 0; i < c.nodes.size(); ++i) {
        const auto& node = c.nodes[i];
        const Group& g = groups[c.find(i)];
        if (node.kind == Connectivity::Node::Kind::Pin && g.wires == 0 && g.pins == 1 && g.labels == 0) {
            const auto* element = static_cast<const Element*>(node.owner);
            issues.push_back({Issue::Kind::UnconnectedPin,
                              "вывод " + element->pins[node.index].name + " элемента " + element->designator + " никуда не подключён",
                              element->id()});
        }
        if (node.kind == Connectivity::Node::Kind::WireVertex && node.wireEnd) {
            // Конец проводника должен упираться во что-то: вывод, метку или другой проводник.
            bool touches = false;
            for (std::size_t j = 0; j < c.nodes.size() && !touches; ++j) {
                if (c.nodes[j].owner == node.owner) continue;
                touches = geometry::almostEqual(c.nodes[j].at, node.at, kTolerance);
            }
            for (const auto* other : document.objectsOf<Wire>()) {
                if (touches || other == node.owner) continue;
                for (std::size_t s = 0; s + 1 < other->points.size(); ++s)
                    touches = touches || onSegment(node.at, other->points[s], other->points[s + 1]);
            }
            if (!touches)
                issues.push_back({Issue::Kind::DanglingWire, "конец проводника ни к чему не присоединён (висит)", node.owner->id()});
        }
    }
    std::set<std::size_t> reported;
    for (std::size_t i = 0; i < c.nodes.size(); ++i) {
        const std::size_t root = c.find(i);
        if (groups[root].names.size() > 1 && reported.insert(root).second) {
            std::string list;
            for (const auto& name : groups[root].names) list += (list.empty() ? "" : ", ") + name;
            issues.push_back({Issue::Kind::LabelConflict, "у одной цепи несколько разных имён: " + list, c.nodes[i].owner->id()});
        }
    }
    std::map<std::string, int> seen;
    for (const auto* element : document.objectsOf<Element>()) {
        if (element->designator.empty())
            issues.push_back({Issue::Kind::EmptyDesignator, "у элемента нет позиционного обозначения", element->id()});
        else if (++seen[element->designator] == 2)
            issues.push_back({Issue::Kind::DuplicateDesignator, "обозначение " + element->designator + " повторяется", element->id()});
    }
    return issues;
}

Result<void> SchematicService::exportElementListCsv(const Document& document, const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "не удалось открыть файл для записи: " + path.string());
    const auto cell = [](const std::string& text) {
        std::string quoted = "\"";
        for (const char ch : text) quoted += ch == '"' ? std::string("\"\"") : std::string(1, ch);
        return quoted + "\"";
    };
    out << "\xEF\xBB\xBF" << "Поз. обозначение;Наименование;Кол.;Примечание\r\n";
    for (const auto& row : elementList(document)) out << cell(row.designators) << ";" << cell(row.name) << ";" << row.count << ";\r\n";
    out.flush();
    if (!out) return makeError(ErrorCode::IoError, "не удалось записать файл: " + path.string());
    return {};
}

}  // namespace cosmocad::schematic
