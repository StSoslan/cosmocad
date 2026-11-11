#include "cosmocad/graphics/Render3D.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cosmocad::graphics {

using geometry::Point3;
using geometry::Vector3;

namespace {

const double kRadians = std::numbers::pi / 180.0;

struct Projected {
    double x, y, depth;  // depth: больше — дальше от наблюдателя
};

struct Projector {
    Vector3 r, u, e;
    Point3 target;
    double zoom, cx, cy;

    Projected operator()(const Point3& p) const {
        const Vector3 d{p.x - target.x, p.y - target.y, p.z - target.z};
        return {cx + geometry::dotProduct(d, r) * zoom, cy - geometry::dotProduct(d, u) * zoom,
                -geometry::dotProduct(d, e)};
    }
};

std::uint32_t shade(std::uint32_t color, double k) {
    const auto channel = [&](int shift) {
        const double v = static_cast<double>((color >> shift) & 0xFF) * k;
        return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0));
    };
    return 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, double t) {
    const auto channel = [&](int shift) {
        const double v = static_cast<double>((a >> shift) & 0xFF) * (1.0 - t) + static_cast<double>((b >> shift) & 0xFF) * t;
        return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0));
    };
    return 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

}  // namespace

Vector3 Camera3D::toEye() const {
    const double y = yawDegrees * kRadians, p = pitchDegrees * kRadians;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

Vector3 Camera3D::right() const {
    const double y = yawDegrees * kRadians;
    return {-std::sin(y), std::cos(y), 0.0};
}

Vector3 Camera3D::up() const { return geometry::crossProduct(toEye(), right()); }

Pick3D Image3D::pick(const std::vector<Body3D>& bodies, int x, int y) const {
    Pick3D out;
    if (x < 0 || y < 0 || x >= width || y >= height) return out;
    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    out.body = bodyAt[index];
    if (out.body >= 0 && static_cast<std::size_t>(out.body) < bodies.size() && faceAt[index] >= 0) {
        const auto& names = bodies[static_cast<std::size_t>(out.body)].mesh->faceNames;
        if (static_cast<std::size_t>(faceAt[index]) < names.size()) out.faceName = names[static_cast<std::size_t>(faceAt[index])];
    }
    return out;
}

Image3D render(const std::vector<Body3D>& bodies, const Camera3D& camera, int width, int height,
               std::uint32_t background) {
    Image3D image;
    image.width = std::max(width, 1);
    image.height = std::max(height, 1);
    const std::size_t count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    image.pixels.assign(count, background);
    image.bodyAt.assign(count, -1);
    image.faceAt.assign(count, -1);
    std::vector<float> depth(count, std::numeric_limits<float>::max());

    const Projector project{camera.right(), camera.up(), camera.toEye(), camera.target, camera.zoom,
                            image.width / 2.0, image.height / 2.0};
    // Свет идёт от наблюдателя, чуть сверху и слева — грани разной ориентации различимы.
    const Vector3 light = geometry::normalized(Vector3{project.e.x + 0.35 * project.u.x - 0.25 * project.r.x,
                                                       project.e.y + 0.35 * project.u.y - 0.25 * project.r.y,
                                                       project.e.z + 0.35 * project.u.z - 0.25 * project.r.z});

    for (std::size_t b = 0; b < bodies.size(); ++b) {
        const geometry::Mesh* mesh = bodies[b].mesh;
        if (!mesh) continue;
        std::vector<char> highlighted(mesh->faceNames.size(), 0);
        for (std::size_t f = 0; f < mesh->faceNames.size(); ++f)
            highlighted[f] = std::find(bodies[b].highlightedFaces.begin(), bodies[b].highlightedFaces.end(),
                                       mesh->faceNames[f]) != bodies[b].highlightedFaces.end();

        for (std::size_t t = 0; t < mesh->triangles.size(); ++t) {
            const auto& tri = mesh->triangles[t];
            const Projected p0 = project(mesh->vertices[tri[0]]), p1 = project(mesh->vertices[tri[1]]),
                            p2 = project(mesh->vertices[tri[2]]);
            const double area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
            if (std::abs(area) < 1e-12) continue;
            const int minX = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
            const int maxX = std::min(image.width - 1, static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
            const int maxY = std::min(image.height - 1, static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
            if (minX > maxX || minY > maxY) continue;

            const std::uint32_t faceIndex = t < mesh->triangleFace.size() ? mesh->triangleFace[t] : 0;
            std::uint32_t base = bodies[b].color;
            if (faceIndex < highlighted.size() && highlighted[faceIndex]) base = mix(base, 0xFFFF9A1Fu, 0.75);
            double brightness[3];
            for (int i = 0; i < 3; ++i) {
                const Vector3& n = mesh->normals[tri[static_cast<std::size_t>(i)]];
                // Обратная сторона грани тоже освещается: при разрезах и ошибках сетки не бывает чёрных дыр.
                brightness[i] = 0.42 + 0.58 * std::abs(geometry::dotProduct(n, light));
            }
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) / area;
                    const double w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) / area;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                    const float z = static_cast<float>(w0 * p0.depth + w1 * p1.depth + w2 * p2.depth);
                    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                              static_cast<std::size_t>(x);
                    if (z >= depth[index]) continue;
                    depth[index] = z;
                    image.pixels[index] = shade(base, w0 * brightness[0] + w1 * brightness[1] + w2 * brightness[2]);
                    image.bodyAt[index] = static_cast<std::int32_t>(b);
                    image.faceAt[index] = static_cast<std::int32_t>(faceIndex);
                }
            }
        }
    }

    // Рёбра тел — тёмные линии поверх граней; ребро, закрытое телом, не рисуется.
    const float bias = static_cast<float>(0.05 + 1.5 / std::max(camera.zoom, 1e-6));
    for (const auto& body : bodies) {
        if (!body.mesh) continue;
        for (const auto& line : body.mesh->edges) {
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const Projected a = project(line[i]), c = project(line[i + 1]);
                const int steps = static_cast<int>(std::ceil(std::max(std::abs(c.x - a.x), std::abs(c.y - a.y)))) + 1;
                if (steps > 20000) continue;
                for (int s = 0; s <= steps; ++s) {
                    const double t = static_cast<double>(s) / steps;
                    const int x = static_cast<int>(std::floor(a.x + (c.x - a.x) * t));
                    const int y = static_cast<int>(std::floor(a.y + (c.y - a.y) * t));
                    if (x < 0 || y < 0 || x >= image.width || y >= image.height) continue;
                    const float z = static_cast<float>(a.depth + (c.depth - a.depth) * t);
                    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                              static_cast<std::size_t>(x);
                    if (z - bias > depth[index]) continue;
                    image.pixels[index] = 0xFF20262Cu;
                }
            }
        }
    }
    return image;
}

Camera3D fitCamera(const std::vector<Body3D>& bodies, int width, int height, double yawDegrees, double pitchDegrees) {
    Camera3D camera;
    camera.yawDegrees = yawDegrees;
    camera.pitchDegrees = pitchDegrees;
    Point3 low{1e300, 1e300, 1e300}, high{-1e300, -1e300, -1e300};
    bool any = false;
    for (const auto& body : bodies) {
        if (!body.mesh) continue;
        for (const auto& p : body.mesh->vertices) {
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
            any = true;
        }
    }
    if (!any) return camera;
    camera.target = {(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
    const Vector3 r = camera.right(), u = camera.up();
    double halfWidth = 1e-6, halfHeight = 1e-6;
    for (const auto& body : bodies) {
        if (!body.mesh) continue;
        for (const auto& p : body.mesh->vertices) {
            const Vector3 d{p.x - camera.target.x, p.y - camera.target.y, p.z - camera.target.z};
            halfWidth = std::max(halfWidth, std::abs(geometry::dotProduct(d, r)));
            halfHeight = std::max(halfHeight, std::abs(geometry::dotProduct(d, u)));
        }
    }
    camera.zoom = 0.85 * std::min(width / (2.0 * halfWidth), height / (2.0 * halfHeight));
    return camera;
}

}  // namespace cosmocad::graphics
