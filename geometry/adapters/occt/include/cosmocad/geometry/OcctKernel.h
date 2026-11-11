// Адаптер геометрического ядра OpenCascade (OCCT).
// Единственное место в проекте, где подключаются заголовки OCCT.
#pragma once

#include <memory>

#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::geometry {

std::unique_ptr<IGeometryKernel> createOcctKernel();

// Версия библиотеки OCCT, с которой собрана программа (для окна «О программе» и отчётов).
const char* occtVersion();

}  // namespace cosmocad::geometry
