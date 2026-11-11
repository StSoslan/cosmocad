// Рисование сцены средствами Qt. Один и тот же код рисует на экране и в PDF.
#pragma once

#include <QPainter>
#include <QPointF>
#include <unordered_set>

#include "cosmocad/core/Uuid.h"
#include "cosmocad/graphics/Scene.h"

class QPrinter;

namespace cosmocad::ui {

struct ScenePaintOptions {
    double scale = 1.0;             // точек устройства на миллиметр
    QPointF origin;                 // положение начала чертежа на устройстве
    double minPenWidth = 1.0;       // наименьшая толщина линии в точках устройства
    const std::unordered_set<core::Uuid>* selection = nullptr;
};

void paintScene(QPainter& painter, const graphics::Scene& scene, const ScenePaintOptions& options);

// Экспорт сцены в векторный PDF в масштабе 1:1 (размер страницы — размер листа).
bool exportPdf(const graphics::Scene& scene, const QString& fileName);

// Печать сцены в масштабе 1:1: левый нижний угол листа совмещается с левым нижним углом бумаги.
// Принтер должен быть настроен на печать без полей (setFullPage), иначе чертёж сдвинется.
bool printScene(const graphics::Scene& scene, QPrinter& printer);

}  // namespace cosmocad::ui
