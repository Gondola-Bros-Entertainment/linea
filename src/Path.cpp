// linea - Path implementation.
#include "linea/Path.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace linea {
namespace {

constexpr double quarterTurn = std::numbers::pi / 2.0;
constexpr double fullTurn = 2.0 * std::numbers::pi;
// An arc that is a whole number of quarter turns, give or take rounding, takes that many pieces.
constexpr double sweepSlack = 1e-9;

// The signed angle from @p u to @p v, in (-pi, pi].
double angleBetween(Vec2 u, Vec2 v) { return std::atan2(cross(u, v), dot(u, v)); }

} // namespace

void Path::ensureStarted() {
    if (!open_)
        moveTo(current_);
}

Path &Path::moveTo(Vec2 to) {
    verbs_.push_back(Verb::move);
    points_.push_back(to);
    start_ = to;
    current_ = to;
    open_ = true;
    return *this;
}

Path &Path::lineTo(Vec2 to) {
    ensureStarted();
    verbs_.push_back(Verb::line);
    points_.push_back(to);
    current_ = to;
    return *this;
}

Path &Path::quadTo(Vec2 control, Vec2 to) {
    ensureStarted();
    verbs_.push_back(Verb::quad);
    points_.push_back(control);
    points_.push_back(to);
    current_ = to;
    return *this;
}

Path &Path::cubicTo(Vec2 control1, Vec2 control2, Vec2 to) {
    ensureStarted();
    verbs_.push_back(Verb::cubic);
    points_.push_back(control1);
    points_.push_back(control2);
    points_.push_back(to);
    current_ = to;
    return *this;
}

Path &Path::arcTo(Vec2 radii, double rotation, bool largeArc, bool clockwise, Vec2 to) {
    ensureStarted();
    const Vec2 from = current_;
    if (from == to)
        return *this;
    double rx = std::abs(radii.x);
    double ry = std::abs(radii.y);
    if (rx == 0.0 || ry == 0.0)
        return lineTo(to);

    // The endpoint to center conversion of the SVG specification, appendix F.6.5.
    const double cs = std::cos(rotation);
    const double sn = std::sin(rotation);
    const Vec2 half = (from - to) * 0.5;
    const Vec2 p{cs * half.x + sn * half.y, -sn * half.x + cs * half.y};
    const double reach = (p.x * p.x) / (rx * rx) + (p.y * p.y) / (ry * ry);
    if (reach > 1.0) {
        rx *= std::sqrt(reach);
        ry *= std::sqrt(reach);
    }
    const double rx2 = rx * rx;
    const double ry2 = ry * ry;
    const double denominator = rx2 * p.y * p.y + ry2 * p.x * p.x;
    double factor = std::sqrt(std::max(0.0, (rx2 * ry2 - denominator) / denominator));
    if (largeArc == clockwise)
        factor = -factor;
    const Vec2 centerPrime{factor * rx * p.y / ry, -factor * ry * p.x / rx};
    const Vec2 middle = (from + to) * 0.5;
    const Vec2 center{cs * centerPrime.x - sn * centerPrime.y + middle.x,
                      sn * centerPrime.x + cs * centerPrime.y + middle.y};
    const Vec2 u{(p.x - centerPrime.x) / rx, (p.y - centerPrime.y) / ry};
    const Vec2 v{(-p.x - centerPrime.x) / rx, (-p.y - centerPrime.y) / ry};
    const double startAngle = angleBetween({1.0, 0.0}, u);
    double sweep = std::fmod(angleBetween(u, v), fullTurn);
    if (!clockwise && sweep > 0.0)
        sweep -= fullTurn;
    else if (clockwise && sweep < 0.0)
        sweep += fullTurn;

    // Each piece spans at most a quarter turn, where a cubic stays within 0.03% of the radius.
    const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / quarterTurn - sweepSlack)));
    const double step = sweep / pieces;
    const double handle = 4.0 / 3.0 * std::tan(step / 4.0);
    const auto onEllipse = [&](Vec2 unit) {
        const Vec2 scaled{unit.x * rx, unit.y * ry};
        return Vec2{cs * scaled.x - sn * scaled.y + center.x, sn * scaled.x + cs * scaled.y + center.y};
    };
    for (int piece = 0; piece < pieces; ++piece) {
        const double a0 = startAngle + step * piece;
        const double a1 = a0 + step;
        const Vec2 e0{std::cos(a0), std::sin(a0)};
        const Vec2 e1{std::cos(a1), std::sin(a1)};
        const Vec2 control1 = onEllipse(e0 + perpendicular(e0) * handle);
        const Vec2 control2 = onEllipse(e1 - perpendicular(e1) * handle);
        // The last piece ends exactly where it was asked to, whatever rounding the angles carry.
        cubicTo(control1, control2, piece + 1 == pieces ? to : onEllipse(e1));
    }
    return *this;
}

Path &Path::close() {
    if (open_) {
        verbs_.push_back(Verb::close);
        current_ = start_;
        open_ = false;
    }
    return *this;
}

Path &Path::append(const Path &other) {
    std::size_t at = 0;
    for (const Verb verb : other.verbs_) {
        const Vec2 *p = other.points_.data() + at;
        switch (verb) {
        case Verb::move:
            moveTo(p[0]);
            break;
        case Verb::line:
            lineTo(p[0]);
            break;
        case Verb::quad:
            quadTo(p[0], p[1]);
            break;
        case Verb::cubic:
            cubicTo(p[0], p[1], p[2]);
            break;
        case Verb::close:
            close();
            break;
        }
        at += static_cast<std::size_t>(pointCount(verb));
    }
    return *this;
}

Path transformed(const Path &path, const Affine &m) {
    Path result;
    std::size_t at = 0;
    const std::vector<Vec2> &points = path.points();
    for (const Verb verb : path.verbs()) {
        const auto map = [&](std::size_t offset) { return apply(m, points[at + offset]); };
        switch (verb) {
        case Verb::move:
            result.moveTo(map(0));
            break;
        case Verb::line:
            result.lineTo(map(0));
            break;
        case Verb::quad:
            result.quadTo(map(0), map(1));
            break;
        case Verb::cubic:
            result.cubicTo(map(0), map(1), map(2));
            break;
        case Verb::close:
            result.close();
            break;
        }
        at += static_cast<std::size_t>(pointCount(verb));
    }
    return result;
}

Bounds controlBounds(const Path &path) {
    const std::vector<Vec2> &points = path.points();
    if (points.empty())
        return {};
    Bounds bounds{points.front(), points.front()};
    for (const Vec2 p : points) {
        bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y)};
        bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y)};
    }
    return bounds;
}

} // namespace linea
