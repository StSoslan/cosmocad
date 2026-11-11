#include "cosmocad/geometry/Geometry2D.h"

#include <algorithm>
#include <numbers>

namespace cosmocad::geometry {

static constexpr double kTwoPi = 2.0 * std::numbers::pi;

void BBox2::expand(Point2 p) {
    if (!valid) {
        min = max = p;
        valid = true;
        return;
    }
    min.x = std::min(min.x, p.x);
    min.y = std::min(min.y, p.y);
    max.x = std::max(max.x, p.x);
    max.y = std::max(max.y, p.y);
}

void BBox2::expand(const BBox2& other) {
    if (!other.valid) return;
    expand(other.min);
    expand(other.max);
}

bool BBox2::contains(Point2 p, double tolerance) const {
    return valid && p.x >= min.x - tolerance && p.x <= max.x + tolerance && p.y >= min.y - tolerance &&
           p.y <= max.y + tolerance;
}

bool BBox2::contains(const BBox2& other, double tolerance) const {
    return other.valid && contains(other.min, tolerance) && contains(other.max, tolerance);
}

BBox2 BBox2::fromCorners(Point2 a, Point2 b) {
    BBox2 box;
    box.expand(a);
    box.expand(b);
    return box;
}

double normalizeAngle(double angle) {
    angle = std::fmod(angle, kTwoPi);
    if (angle < 0.0) angle += kTwoPi;
    if (angle >= kTwoPi) angle = 0.0;
    return angle;
}

bool angleOnArc(double angle, double start, double end) {
    double sweep = normalizeAngle(end - start);
    if (sweep < kAngularTolerance) sweep = kTwoPi;  // совпадающие концы — полная окружность
    double offset = normalizeAngle(angle - start);
    return offset <= sweep + kAngularTolerance || offset >= kTwoPi - kAngularTolerance;
}

Point2 pointOnCircle(Point2 center, double radius, double angle) {
    return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

double distanceToSegment(Point2 p, Point2 a, Point2 b) {
    Point2 ab = b - a;
    double lengthSquared = dot(ab, ab);
    if (lengthSquared < kLinearTolerance * kLinearTolerance) return distance(p, a);
    double t = std::clamp(dot(p - a, ab) / lengthSquared, 0.0, 1.0);
    return distance(p, a + ab * t);
}

double distanceToCircle(Point2 p, Point2 center, double radius) {
    return std::abs(distance(p, center) - radius);
}

double distanceToArc(Point2 p, Point2 center, double radius, double start, double end) {
    double angle = std::atan2(p.y - center.y, p.x - center.x);
    if (angleOnArc(angle, start, end)) return distanceToCircle(p, center, radius);
    return std::min(distance(p, pointOnCircle(center, radius, start)), distance(p, pointOnCircle(center, radius, end)));
}

BBox2 arcBounds(Point2 center, double radius, double start, double end) {
    BBox2 box;
    box.expand(pointOnCircle(center, radius, start));
    box.expand(pointOnCircle(center, radius, end));
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        double angle = quadrant * std::numbers::pi / 2.0;
        if (angleOnArc(angle, start, end)) box.expand(pointOnCircle(center, radius, angle));
    }
    return box;
}

std::optional<Point2> segmentIntersection(Point2 a1, Point2 a2, Point2 b1, Point2 b2) {
    Point2 r = a2 - a1;
    Point2 s = b2 - b1;
    double denominator = cross(r, s);
    if (std::abs(denominator) < 1e-12) return std::nullopt;
    double t = cross(b1 - a1, s) / denominator;
    double u = cross(b1 - a1, r) / denominator;
    const double eps = 1e-9;
    if (t < -eps || t > 1.0 + eps || u < -eps || u > 1.0 + eps) return std::nullopt;
    return a1 + r * t;
}

std::optional<Point2> lineIntersection(Point2 a1, Point2 a2, Point2 b1, Point2 b2) {
    Point2 r = a2 - a1;
    Point2 s = b2 - b1;
    double denominator = cross(r, s);
    if (std::abs(denominator) < 1e-12 * (length(r) * length(s) + 1e-300)) return std::nullopt;
    double t = cross(b1 - a1, s) / denominator;
    return a1 + r * t;
}

std::vector<double> segmentCircleParams(Point2 a, Point2 b, Point2 center, double radius) {
    std::vector<double> out;
    Point2 d = b - a;
    Point2 f = a - center;
    double A = dot(d, d);
    if (A < 1e-24) return out;
    double B = 2.0 * dot(f, d);
    double C = dot(f, f) - radius * radius;
    double discriminant = B * B - 4.0 * A * C;
    if (discriminant < 0.0) return out;
    double root = std::sqrt(discriminant);
    const double eps = 1e-9;
    for (double t : {(-B - root) / (2.0 * A), (-B + root) / (2.0 * A)})
        if (t >= -eps && t <= 1.0 + eps) out.push_back(std::clamp(t, 0.0, 1.0));
    if (out.size() == 2 && std::abs(out[0] - out[1]) < 1e-9) out.pop_back();  // касание
    return out;
}

std::vector<Point2> tessellateArc(Point2 center, double radius, double start, double end, double tolerance) {
    double sweep = normalizeAngle(end - start);
    if (sweep < kAngularTolerance) sweep = kTwoPi;
    // Отклонение хорды от дуги: r·(1 − cos(шаг/2)) ≤ tolerance.
    double step = radius > tolerance ? 2.0 * std::acos(1.0 - tolerance / radius) : kTwoPi / 8.0;
    int segments = std::clamp(static_cast<int>(std::ceil(sweep / step)), 4, 720);
    std::vector<Point2> points;
    points.reserve(static_cast<std::size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) points.push_back(pointOnCircle(center, radius, start + sweep * i / segments));
    return points;
}

Point2 closestPointOnSegment(Point2 p, Point2 a, Point2 b) {
    Point2 ab = b - a;
    double lengthSquared = dot(ab, ab);
    if (lengthSquared < kLinearTolerance * kLinearTolerance) return a;
    double t = std::clamp(dot(p - a, ab) / lengthSquared, 0.0, 1.0);
    return a + ab * t;
}

Transform2 Transform2::translation(Point2 delta) {
    Transform2 t;
    t.tx = delta.x;
    t.ty = delta.y;
    return t;
}

Transform2 Transform2::rotation(Point2 center, double angle) {
    Transform2 t;
    t.a = std::cos(angle);
    t.b = std::sin(angle);
    t.tx = center.x - (t.a * center.x - t.b * center.y);
    t.ty = center.y - (t.b * center.x + t.a * center.y);
    return t;
}

Transform2 Transform2::scaling(Point2 center, double factor) {
    Transform2 t;
    t.a = factor;
    t.tx = center.x * (1.0 - factor);
    t.ty = center.y * (1.0 - factor);
    return t;
}

Transform2 Transform2::mirror(Point2 axisA, Point2 axisB) {
    Transform2 t;
    t.mirrored = true;
    double alpha = std::atan2(axisB.y - axisA.y, axisB.x - axisA.x);
    t.a = std::cos(2.0 * alpha);
    t.b = std::sin(2.0 * alpha);
    t.tx = axisA.x - (t.a * axisA.x + t.b * axisA.y);
    t.ty = axisA.y - (t.b * axisA.x - t.a * axisA.y);
    return t;
}

Point2 Transform2::apply(Point2 p) const {
    if (mirrored) return {a * p.x + b * p.y + tx, b * p.x - a * p.y + ty};
    return {a * p.x - b * p.y + tx, b * p.x + a * p.y + ty};
}

}  // namespace cosmocad::geometry
