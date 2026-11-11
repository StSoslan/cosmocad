#include "ScenePainter.h"

#include <QFont>
#include <QPageSize>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPrinter>
#include <algorithm>
#include <cmath>

#include "cosmocad/core/Units.h"

namespace cosmocad::ui {

using graphics::SceneItem;

void paintScene(QPainter& painter, const graphics::Scene& scene, const ScenePaintOptions& options) {
    const double k = options.scale;
    auto device = [&](geometry::Point2 p) { return QPointF(options.origin.x() + p.x * k, options.origin.y() - p.y * k); };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const auto& item : scene.items) {
        bool selected = options.selection && !item.source.isNil() && options.selection->count(item.source) > 0;
        QColor color = selected ? QColor(0, 110, 230) : QColor(QString::fromStdString(item.color));
        if (!color.isValid()) color = Qt::black;

        double penWidth = std::max(options.minPenWidth, item.width * k);
        if (selected) penWidth += 1.0;
        QPen pen(color, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (!item.pattern.empty()) {
            QList<qreal> dashes;
            for (double length : item.pattern) dashes << std::max(0.5, length * k / penWidth);
            if (dashes.size() % 2 == 1) dashes << dashes.last();
            pen.setDashPattern(dashes);
            pen.setCapStyle(Qt::FlatCap);
        }
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        switch (item.kind) {
            case SceneItem::Kind::Line:
                painter.drawLine(device(item.a), device(item.b));
                break;
            case SceneItem::Kind::Circle: {
                double r = item.radius * k;
                painter.drawEllipse(device(item.a), r, r);
                break;
            }
            case SceneItem::Kind::Arc: {
                double r = item.radius * k;
                QPointF c = device(item.a);
                QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
                double start = core::radiansToDegrees(item.startAngle);
                double span = core::radiansToDegrees(geometry::normalizeAngle(item.endAngle - item.startAngle));
                if (span < 1e-9) span = 360.0;
                QPainterPath path;
                path.arcMoveTo(box, start);
                path.arcTo(box, start, span);  // положительный угол — против часовой стрелки
                painter.drawPath(path);
                break;
            }
            case SceneItem::Kind::Text: {
                // Текст рисуется контуром: одинаково на экране и в PDF при любом масштабе.
                QFont font(QStringLiteral("sans-serif"));
                font.setPixelSize(100);
                QPainterPath path;
                path.addText(0, 0, font, QString::fromStdString(item.text));
                double factor = (item.textHeight / 0.7) * k / 100.0;  // высота прописной ≈ 0,7 кегля
                QTransform transform;
                QPointF anchor = device(item.a);
                transform.translate(anchor.x(), anchor.y());
                transform.rotate(-core::radiansToDegrees(item.textAngle));
                transform.scale(factor, factor);
                if (item.textCentered) transform.translate(-path.boundingRect().width() / 2.0, 0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPath(transform.map(path));
                break;
            }
        }
    }
    painter.restore();
}

bool exportPdf(const graphics::Scene& scene, const QString& fileName) {
    double width = scene.sheetWidth, height = scene.sheetHeight;
    geometry::Point2 origin{0.0, 0.0};
    if (width <= 0.0 || height <= 0.0) {
        if (!scene.bounds.valid) return false;
        const double margin = 10.0;
        origin = {scene.bounds.min.x - margin, scene.bounds.min.y - margin};
        width = scene.bounds.width() + 2 * margin;
        height = scene.bounds.height() + 2 * margin;
    }

    QPdfWriter writer(fileName);
    writer.setResolution(1200);
    writer.setPageSize(QPageSize(QSizeF(width, height), QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setCreator(QStringLiteral("CosmoCad"));

    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    ScenePaintOptions options;
    options.scale = writer.resolution() / 25.4;  // точек на миллиметр — масштаб 1:1
    options.origin = QPointF(-origin.x * options.scale, (height + origin.y) * options.scale);
    options.minPenWidth = 0.0;
    paintScene(painter, scene, options);
    return painter.end();
}

bool printScene(const graphics::Scene& scene, QPrinter& printer) {
    QPainter painter(&printer);
    if (!painter.isActive()) return false;
    const double scale = printer.resolution() / 25.4;  // точек принтера на миллиметр — масштаб 1:1
    const double paperHeight = printer.paperRect(QPrinter::Millimeter).height();
    ScenePaintOptions options;
    options.scale = scale;
    // Начало координат чертежа — левый нижний угол бумаги.
    options.origin = QPointF(0.0, paperHeight * scale);
    options.minPenWidth = 0.0;
    paintScene(painter, scene, options);
    return painter.end();
}

}  // namespace cosmocad::ui
