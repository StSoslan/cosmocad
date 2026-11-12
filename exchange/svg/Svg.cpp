#include <cstddef>
#include <string>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <ostream>

#include "cosmocad/core/Units.h"
#include "cosmocad/exchange/Exchange.h"

namespace cosmocad::exchange {

using graphics::SceneItem;

namespace {

std::string escapeXml(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

// Цвет берётся из документа; допускаем только вид #rrggbb, иначе чёрный.
std::string safeColor(const std::string& color) {
    if (color.size() == 7 && color[0] == '#') {
        bool hex = true;
        for (std::size_t i = 1; i < 7; ++i) hex = hex && std::isxdigit(static_cast<unsigned char>(color[i]));
        if (hex) return color;
    }
    return "#000000";
}

}  // namespace

core::Result<void> writeSvg(const graphics::Scene& scene, std::ostream& out) {
    double originX = 0.0, originY = 0.0, width = scene.sheetWidth, height = scene.sheetHeight;
    if (width <= 0.0 || height <= 0.0) {
        const double margin = 10.0;
        if (scene.bounds.valid) {
            originX = scene.bounds.min.x - margin;
            originY = scene.bounds.min.y - margin;
            width = scene.bounds.width() + 2 * margin;
            height = scene.bounds.height() + 2 * margin;
        } else {
            width = height = 100.0;
        }
    }
    auto X = [&](double x) { return x - originX; };
    auto Y = [&](double y) { return height - (y - originY); };  // в SVG ось Y направлена вниз

    out << std::setprecision(10);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "mm\" height=\"" << height
        << "mm\" viewBox=\"0 0 " << width << ' ' << height << "\">\n";
    out << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height << "\" fill=\"#ffffff\"/>\n";

    for (const auto& item : scene.items) {
        std::string stroke = " fill=\"none\" stroke=\"" + safeColor(item.color) + "\" stroke-linecap=\"round\"";
        auto style = [&] {
            out << stroke << " stroke-width=\"" << item.width << "\"";
            if (!item.pattern.empty()) {
                out << " stroke-dasharray=\"";
                for (std::size_t i = 0; i < item.pattern.size(); ++i) out << (i ? " " : "") << item.pattern[i];
                out << "\"";
            }
        };
        switch (item.kind) {
            case SceneItem::Kind::Line:
                out << "<line x1=\"" << X(item.a.x) << "\" y1=\"" << Y(item.a.y) << "\" x2=\"" << X(item.b.x)
                    << "\" y2=\"" << Y(item.b.y) << "\"";
                style();
                out << "/>\n";
                break;
            case SceneItem::Kind::Circle:
                out << "<circle cx=\"" << X(item.a.x) << "\" cy=\"" << Y(item.a.y) << "\" r=\"" << item.radius << "\"";
                style();
                out << "/>\n";
                break;
            case SceneItem::Kind::Arc: {
                auto start = geometry::pointOnCircle(item.a, item.radius, item.startAngle);
                auto end = geometry::pointOnCircle(item.a, item.radius, item.endAngle);
                double sweep = geometry::normalizeAngle(item.endAngle - item.startAngle);
                int largeArc = sweep > std::numbers::pi ? 1 : 0;
                // Дуга против часовой стрелки на чертеже остаётся такой же на экране: sweep-flag = 0.
                out << "<path d=\"M " << X(start.x) << ' ' << Y(start.y) << " A " << item.radius << ' ' << item.radius
                    << " 0 " << largeArc << " 0 " << X(end.x) << ' ' << Y(end.y) << "\"";
                style();
                out << "/>\n";
                break;
            }
            case SceneItem::Kind::Text: {
                // Высота шрифта на чертеже — высота прописной буквы; она около 0,7 кегля.
                double fontSize = item.textHeight / 0.7;
                out << "<text transform=\"translate(" << X(item.a.x) << ' ' << Y(item.a.y) << ") rotate("
                    << -core::radiansToDegrees(item.textAngle) << ")\" font-family=\"sans-serif\" font-size=\""
                    << fontSize << "\" fill=\"" << safeColor(item.color) << "\"";
                if (item.textCentered) out << " text-anchor=\"middle\"";
                out << '>' << escapeXml(item.text) << "</text>\n";
                break;
            }
        }
    }
    out << "</svg>\n";
    if (!out) return core::makeError(core::ErrorCode::IoError, "ошибка записи SVG");
    return {};
}

}  // namespace cosmocad::exchange
