// linea - points and affine transforms.
#pragma once

#include <cmath>

namespace linea {

// A point or offset, x right and y down.
struct Vec2 {
    double x{};
    double y{};
    friend constexpr bool operator==(Vec2, Vec2) = default;
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
constexpr Vec2 operator*(Vec2 a, double s) { return {a.x * s, a.y * s}; }
constexpr Vec2 operator*(double s, Vec2 a) { return {a.x * s, a.y * s}; }
constexpr Vec2 operator/(Vec2 a, double s) { return {a.x / s, a.y / s}; }

constexpr double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
// The z of the 3D cross product: positive when b turns clockwise from a on a y-down canvas.
constexpr double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double length(Vec2 a) { return std::hypot(a.x, a.y); }
// a scaled to length 1, or zero when a is zero.
inline Vec2 normalized(Vec2 a) {
    const double l = length(a);
    return l > 0.0 ? a / l : Vec2{};
}
// a turned a quarter turn clockwise on a y-down canvas.
constexpr Vec2 perpendicular(Vec2 a) { return {-a.y, a.x}; }
constexpr Vec2 lerp(Vec2 a, Vec2 b, double t) { return a + (b - a) * t; }
// The point at @p radius from @p center in direction @p angle, radians clockwise from +x.
inline Vec2 polar(Vec2 center, double radius, double angle) {
    return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

// An affine transform, as SVG's matrix(a b c d e f): x' = a x + c y + e and y' = b x + d y + f.
struct Affine {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
    friend constexpr bool operator==(const Affine &, const Affine &) = default;
};

constexpr Affine identity() { return {}; }
constexpr Affine translate(Vec2 by) { return {1.0, 0.0, 0.0, 1.0, by.x, by.y}; }
constexpr Affine scale(double sx, double sy) { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }
constexpr Affine scale(double s) { return scale(s, s); }
// A rotation by @p angle radians, clockwise on a y-down canvas.
inline Affine rotate(double angle) {
    const double cs = std::cos(angle);
    const double sn = std::sin(angle);
    return {cs, sn, -sn, cs, 0.0, 0.0};
}
inline Affine skewX(double angle) { return {1.0, 0.0, std::tan(angle), 1.0, 0.0, 0.0}; }
inline Affine skewY(double angle) { return {1.0, std::tan(angle), 0.0, 1.0, 0.0, 0.0}; }

// The transform that applies @p second after @p first: (second * first)(p) == second(first(p)).
constexpr Affine operator*(const Affine &second, const Affine &first) {
    return {second.a * first.a + second.c * first.b,
            second.b * first.a + second.d * first.b,
            second.a * first.c + second.c * first.d,
            second.b * first.c + second.d * first.d,
            second.a * first.e + second.c * first.f + second.e,
            second.b * first.e + second.d * first.f + second.f};
}

constexpr Vec2 apply(const Affine &m, Vec2 p) { return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f}; }
// @p v moved by the transform's linear part only, as a direction or an offset is.
constexpr Vec2 applyLinear(const Affine &m, Vec2 v) { return {m.a * v.x + m.c * v.y, m.b * v.x + m.d * v.y}; }
constexpr double determinant(const Affine &m) { return m.a * m.d - m.b * m.c; }

// A rotation by @p angle radians about @p center.
inline Affine rotateAround(double angle, Vec2 center) { return translate(center) * rotate(angle) * translate(-center); }

// The transform that undoes @p m. A transform whose determinant is 0 collapses the plane and has no inverse; this
// returns the identity for one.
Affine inverse(const Affine &m);

// How far @p m stretches lengths at most: the larger singular value of its linear part.
double maxScale(const Affine &m);

} // namespace linea
