// linea - Stroke implementation. A stroke is the union of a quad for every segment, a join at every corner and a cap at
// every open end. Each piece winds clockwise, so the nonzero rule fills their union however they overlap.
#include "linea/Stroke.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace linea {
namespace {

// Round joins and caps take at least this many sides, and at most the other, however small or large they are.
constexpr int fewestSides = 8;
constexpr int mostSides = 256;
// Segments that turn by less than this, in radians, meet in a straight line and need no join.
constexpr double straight = 1e-9;

void add(std::vector<Contour> &out, std::vector<Vec2> points) {
    Contour piece{std::move(points), true};
    if (twiceSignedArea(piece) < 0.0)
        std::reverse(piece.points.begin(), piece.points.end());
    out.push_back(std::move(piece));
}

// A polygon with the same area as the circle: its corners lie a little outside and its edges a little inside, where an
// inscribed polygon would lie wholly inside and leave every round join and cap thin.
void addCircle(std::vector<Contour> &out, Vec2 center, double radius, double tolerance) {
    // A chord of a circle of radius r through an angle a strays r (1 - cos(a / 2)) from the arc. Equal areas then put
    // the corners 2/3 of that outside the circle and the edges' middles 1/3 of it inside.
    int sides = fewestSides;
    if (tolerance < radius)
        sides = static_cast<int>(std::clamp(std::ceil(std::numbers::pi / std::acos(1.0 - tolerance / radius)),
                                            static_cast<double>(fewestSides), static_cast<double>(mostSides)));
    const double step = 2.0 * std::numbers::pi / sides;
    const double corner = radius * std::sqrt(step / std::sin(step));
    std::vector<Vec2> points;
    points.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i)
        points.push_back(polar(center, corner, step * i));
    add(out, std::move(points));
}

void addSegment(std::vector<Contour> &out, Vec2 a, Vec2 b, double half) {
    const Vec2 normal = perpendicular(normalized(b - a)) * half;
    add(out, {a + normal, b + normal, b - normal, a - normal});
}

// The join at @p corner between a segment along @p before and the next along @p after, both unit vectors.
void addJoin(std::vector<Contour> &out, Vec2 corner, Vec2 before, Vec2 after, const StrokeStyle &style, double half,
             double tolerance) {
    const double turn = cross(before, after);
    if (std::abs(turn) < straight && dot(before, after) > 0.0)
        return;
    if (style.join == Join::round) {
        addCircle(out, corner, half, tolerance);
        return;
    }
    // The outer side of the corner is the one the path turns away from.
    const double side = turn > 0.0 ? -1.0 : 1.0;
    const Vec2 out0 = perpendicular(before) * (half * side);
    const Vec2 out1 = perpendicular(after) * (half * side);
    if (style.join == Join::miter) {
        const Vec2 bisector = normalized(out0 + out1);
        const double cosHalf = dot(bisector, out0) / half;
        if (cosHalf > 0.0 && 1.0 / cosHalf <= style.miterLimit) {
            add(out, {corner, corner + out0, corner + bisector * (half / cosHalf), corner + out1});
            return;
        }
    }
    add(out, {corner, corner + out0, corner + out1});
}

// The cap at @p end of an open stroke, which leaves the end along the unit vector @p outward.
void addCap(std::vector<Contour> &out, Vec2 end, Vec2 outward, const StrokeStyle &style, double half,
            double tolerance) {
    switch (style.cap) {
    case Cap::butt:
        return;
    case Cap::round:
        addCircle(out, end, half, tolerance);
        return;
    case Cap::square: {
        const Vec2 normal = perpendicular(outward) * half;
        const Vec2 beyond = end + outward * half;
        add(out, {end + normal, beyond + normal, beyond - normal, end - normal});
        return;
    }
    }
}

} // namespace

std::vector<Contour> strokeOutline(const std::vector<Contour> &contours, const StrokeStyle &style, double tolerance) {
    std::vector<Contour> out;
    const double half = style.width / 2.0;
    if (!(half > 0.0))
        return out;
    for (const Contour &contour : contours) {
        // Repeated points have no direction to stroke along.
        std::vector<Vec2> p;
        for (const Vec2 point : contour.points)
            if (p.empty() || point != p.back())
                p.push_back(point);
        const bool closed = contour.closed && p.size() > 2;
        if (closed && p.back() == p.front())
            p.pop_back();
        if (p.size() == 1) {
            // A dot: only its caps show, as a round or square pen mark.
            if (style.cap == Cap::round)
                addCircle(out, p.front(), half, tolerance);
            else if (style.cap == Cap::square)
                add(out, {p.front() + Vec2{-half, -half}, p.front() + Vec2{half, -half}, p.front() + Vec2{half, half},
                          p.front() + Vec2{-half, half}});
            continue;
        }
        if (p.empty())
            continue;
        const std::size_t n = p.size();
        const std::size_t segments = closed ? n : n - 1;
        for (std::size_t i = 0; i < segments; ++i)
            addSegment(out, p[i], p[(i + 1) % n], half);
        const auto direction = [&](std::size_t from) { return normalized(p[(from + 1) % n] - p[from]); };
        for (std::size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i)
            addJoin(out, p[i], direction((i + n - 1) % n), direction(i), style, half, tolerance);
        if (!closed) {
            addCap(out, p.front(), -direction(0), style, half, tolerance);
            addCap(out, p.back(), direction(n - 2), style, half, tolerance);
        }
    }
    return out;
}

} // namespace linea
