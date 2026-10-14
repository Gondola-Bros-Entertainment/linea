// linea - Path implementation.
#include "linea/Path.hpp"

#include "linea/Flatten.hpp"

#include "Roots.hpp"

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

// Grows @p bounds to hold @p p.
void include(Bounds &bounds, Vec2 p) {
    bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y)};
    bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y)};
}

// Grows @p bounds to hold the points of a curve, found by @p pointAt, where the polynomial a t^2 + b t + c that its
// derivative is a multiple of is zero in x or in y.
template <typename PointAt> void includeTurns(Bounds &bounds, Vec2 a, Vec2 b, Vec2 c, PointAt pointAt) {
    const auto turn = [&](double t) { include(bounds, pointAt(t)); };
    detail::forEachUnitRoot(a.x, b.x, c.x, turn);
    detail::forEachUnitRoot(a.y, b.y, c.y, turn);
}

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
    // Ends or radii so far apart that the arithmetic overflows a double leave no angle to sweep: the arc is drawn as
    // the line between its ends, which an ellipse too large to compute already all but is.
    if (!std::isfinite(sweep) || !std::isfinite(startAngle))
        return lineTo(to);

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

Path reversed(const Path &path) {
    const std::vector<Verb> &verbs = path.verbs();
    const std::vector<Vec2> &points = path.points();
    Path result;
    // Walking backward, verb and end count the verbs and points not yet visited. Every subpath starts with a move, so
    // each segment starts at the point just before its own: the last point of the verb before it.
    std::size_t verb = verbs.size();
    std::size_t end = points.size();
    while (verb > 0) {
        const bool closed = verbs[verb - 1] == Verb::close;
        if (closed)
            --verb;
        result.moveTo(points[end - 1]);
        // Between a subpath's move and its close lie only lines and curves.
        for (; verbs[verb - 1] != Verb::move; --verb) {
            const Verb segment = verbs[verb - 1];
            const std::size_t at = end - static_cast<std::size_t>(pointCount(segment));
            const Vec2 from = points[at - 1];
            if (segment == Verb::line)
                result.lineTo(from);
            else if (segment == Verb::quad)
                result.quadTo(points[at], from);
            else
                result.cubicTo(points[at + 1], points[at], from);
            end = at;
        }
        // Step past the subpath's move, where the reversed subpath has just ended.
        --verb;
        --end;
        if (closed)
            result.close();
    }
    return result;
}

Bounds controlBounds(const Path &path) {
    const std::vector<Vec2> &points = path.points();
    if (points.empty())
        return {};
    Bounds bounds{points.front(), points.front()};
    for (const Vec2 p : points)
        include(bounds, p);
    return bounds;
}

Bounds tightBounds(const Path &path) {
    const std::vector<Vec2> &points = path.points();
    if (points.empty())
        return {};
    Bounds bounds{points.front(), points.front()};
    std::size_t at = 0;
    Vec2 current{};
    for (const Verb verb : path.verbs()) {
        const Vec2 *p = points.data() + at;
        switch (verb) {
        case Verb::move:
        case Verb::line:
            include(bounds, p[0]);
            current = p[0];
            break;
        case Verb::quad: {
            // A quadratic's derivative is 2 (d0 + (d1 - d0) t), with d0 = p1 - p0 and d1 = p2 - p1.
            const Vec2 d0 = p[0] - current;
            const Vec2 d1 = p[1] - p[0];
            includeTurns(bounds, {}, d1 - d0, d0, [&](double t) { return evalQuad(current, p[0], p[1], t); });
            include(bounds, p[1]);
            current = p[1];
            break;
        }
        case Verb::cubic: {
            // A cubic's derivative is 3 ((d0 - 2 d1 + d2) t^2 + 2 (d1 - d0) t + d0), with d0 = p1 - p0, d1 = p2 - p1
            // and d2 = p3 - p2.
            const Vec2 d0 = p[0] - current;
            const Vec2 d1 = p[1] - p[0];
            const Vec2 d2 = p[2] - p[1];
            includeTurns(bounds, d0 - d1 * 2.0 + d2, (d1 - d0) * 2.0, d0,
                         [&](double t) { return evalCubic(current, p[0], p[1], p[2], t); });
            include(bounds, p[2]);
            current = p[2];
            break;
        }
        case Verb::close:
            break;
        }
        at += static_cast<std::size_t>(pointCount(verb));
    }
    // A curve lies within its control points, but rounding in evaluating it where it turns can land just past them, so
    // the box is cut back to theirs.
    const Bounds control = controlBounds(path);
    return {{std::max(bounds.min.x, control.min.x), std::max(bounds.min.y, control.min.y)},
            {std::min(bounds.max.x, control.max.x), std::min(bounds.max.y, control.max.y)}};
}

} // namespace linea
