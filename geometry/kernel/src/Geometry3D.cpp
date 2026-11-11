#include <cmath>

#include "cosmocad/geometry/IGeometryKernel.h"

namespace cosmocad::geometry {

Vector3 normalized(const Vector3& v) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-300) return Vector3{0.0, 0.0, 1.0};
    return Vector3{v.x / len, v.y / len, v.z / len};
}

Vector3 crossProduct(const Vector3& a, const Vector3& b) {
    return Vector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double dotProduct(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Point3 Placement::apply(const Point3& p) const {
    return Point3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                  m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

Vector3 Placement::rotate(const Vector3& v) const {
    return Vector3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
                   m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

Placement Placement::operator*(const Placement& inner) const {
    Placement out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) sum += m[row * 4 + k] * inner.m[k * 4 + col];
            out.m[row * 4 + col] = sum;
        }
        double sum = m[row * 4 + 3];
        for (int k = 0; k < 3; ++k) sum += m[row * 4 + k] * inner.m[k * 4 + 3];
        out.m[row * 4 + 3] = sum;
    }
    return out;
}

Placement Placement::translation(double dx, double dy, double dz) {
    Placement out;
    out.m[3] = dx;
    out.m[7] = dy;
    out.m[11] = dz;
    return out;
}

Placement Placement::rotation(const Point3& point, const Vector3& axis, double angle) {
    const Vector3 k = normalized(axis);
    const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    Placement r;
    r.m = {t * k.x * k.x + c,       t * k.x * k.y - s * k.z, t * k.x * k.z + s * k.y, 0.0,
           t * k.x * k.y + s * k.z, t * k.y * k.y + c,       t * k.y * k.z - s * k.x, 0.0,
           t * k.x * k.z - s * k.y, t * k.y * k.z + s * k.x, t * k.z * k.z + c,       0.0};
    // Поворот вокруг точки: перенести в начало, повернуть, вернуть.
    const Point3 moved = r.apply(point);
    r.m[3] = point.x - moved.x;
    r.m[7] = point.y - moved.y;
    r.m[11] = point.z - moved.z;
    return r;
}

}  // namespace cosmocad::geometry
