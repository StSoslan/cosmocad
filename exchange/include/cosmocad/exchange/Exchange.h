// Обмен с другими программами. Что именно поддержано в каждом формате — см. «паспорта»
// форматов в docs/exchange/. Формат без паспорта и тестов поддержанным не считается.
#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "cosmocad/core/Result.h"
#include "cosmocad/document/Object.h"
#include "cosmocad/graphics/Scene.h"

namespace cosmocad::exchange {

// --- DXF (ASCII, версия R12 / AC1009) ---

core::Result<void> writeDxf(const graphics::Scene& scene, std::ostream& out);

struct DxfImport {
    std::vector<std::unique_ptr<document::Object>> objects;  // слои и элементы
    std::size_t skippedEntities = 0;                         // элементы неподдерживаемых типов
};

// Читает LINE, CIRCLE, ARC, TEXT. Повреждённый файл даёт ошибку, а не сбой программы.
core::Result<DxfImport> readDxf(std::istream& in);

// --- SVG (только запись) ---

core::Result<void> writeSvg(const graphics::Scene& scene, std::ostream& out);

}  // namespace cosmocad::exchange
