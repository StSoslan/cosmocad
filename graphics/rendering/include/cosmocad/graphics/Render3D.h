// Показ 3D-тел: программная отрисовка в картинку с буфером глубины.
// Без Qt и без OpenGL: работает везде, где работает программа, и проверяется автотестами.
// Для больших сборок понадобится отрисовка на видеокарте (см. docs/risks.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::graphics {

// Камера: смотрит на точку target под углами yaw (вокруг вертикали Z) и pitch (возвышение).
// Проекция параллельная, zoom — пикселей на миллиметр.
struct Camera3D {
    double yawDegrees = -60.0;
    double pitchDegrees = 30.0;
    double zoom = 4.0;
    geometry::Point3 target;

    geometry::Vector3 toEye() const;  // от цели к наблюдателю
    geometry::Vector3 right() const;
    geometry::Vector3 up() const;
};

struct Body3D {
    const geometry::Mesh* mesh = nullptr;
    std::uint32_t color = 0xFFB8C4D0;           // 0xAARRGGBB
    std::vector<std::string> highlightedFaces;  // имена выделенных граней
};

// Что находится под пикселем.
struct Pick3D {
    int body = -1;  // номер тела или -1 — фон
    std::string faceName;
};

struct Image3D {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;  // 0xAARRGGBB, построчно сверху вниз
    std::vector<std::int32_t> bodyAt;   // номер тела под пикселем или -1
    std::vector<std::int32_t> faceAt;   // номер грани (в Mesh::faceNames тела) или -1

    Pick3D pick(const std::vector<Body3D>& bodies, int x, int y) const;
};

Image3D render(const std::vector<Body3D>& bodies, const Camera3D& camera, int width, int height,
               std::uint32_t background = 0xFFF4F6F8);

// Камера, при которой все тела целиком видны в окне заданного размера.
Camera3D fitCamera(const std::vector<Body3D>& bodies, int width, int height, double yawDegrees, double pitchDegrees);

}  // namespace cosmocad::graphics
