// linea - Flatten implementation.
#include "linea/Flatten.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace linea {
namespace {

// A curve whose bound asks for more segments than this takes this many, so that coordinates far beyond any canvas
// cannot exhaust memory.
constexpr int mostSegments = 4096;

// Segments enough for a curve whose second derivative never exceeds @p bend: a uniform split into n pieces strays at
// most bend / (8 n^2) from the curve.
int segmentsFor(double bend, double tolerance) {
    if (!(bend > 0.0) || !(tolerance > 0.0))
        return 1;
    const double wanted = std::ceil(std::sqrt(bend / (8.0 * tolerance)));
    return static_cast<int>(std::clamp(wanted, 1.0, static_cast<double>(mostSegments)));
}

// The point a fraction @p t of the way from @p a to @p b, given @p s = 1 - t. Weighting both ends, rather than stepping
// from a toward b, lands exactly on b at t = 1 as well as on a at t = 0.
Vec2 mix(Vec2 a, Vec2 b, double s, double t) { return a * s + b * t; }

// The square of the distance from @p p to the segment from @p a to @p b.
double squaredDistanceToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const Vec2 ap = p - a;
    const double span = dot(ab, ab);
    const double along = span > 0.0 ? std::clamp(dot(ap, ab) / span, 0.0, 1.0) : 0.0;
    const Vec2 off = ap - ab * along;
    return dot(off, off);
}

// The index of the point in @p points farthest from @p from, the first of them on a tie.
std::size_t farthestFrom(const std::vector<Vec2> &points, Vec2 from) {
    const auto farthest = std::max_element(points.begin(), points.end(), [&](Vec2 a, Vec2 b) {
        return dot(a - from, a - from) < dot(b - from, b - from);
    });
    return static_cast<std::size_t>(farthest - points.begin());
}

// Marks in @p keep the points that Ramer-Douglas-Peucker keeps strictly between indices @p first and @p last, both kept
// already: the point farthest from the segment between them, if that is farther than @p tolerance, and then the same
// on either side of it. The distance is to the segment, not the line through it, so that a point beyond either end
// of the segment is kept even when it lies on that line. Indices past the last point wrap around to the first, as a
// closed contour's do, and a stack in place of recursion keeps a long, winding contour from exhausting the call stack.
void keepBetween(const std::vector<Vec2> &points, std::size_t first, std::size_t last, double tolerance,
                 std::vector<bool> &keep) {
    const std::size_t n = points.size();
    const auto at = [&](std::size_t i) { return points[i % n]; };
    std::vector<std::pair<std::size_t, std::size_t>> pending{{first, last}};
    while (!pending.empty()) {
        const auto [from, to] = pending.back();
        pending.pop_back();
        if (to - from < 2)
            continue;
        const Vec2 a = at(from);
        const Vec2 b = at(to);
        std::size_t farthest = from + 1;
        double farthestSquared = squaredDistanceToSegment(at(farthest), a, b);
        for (std::size_t i = from + 2; i < to; ++i) {
            const double squared = squaredDistanceToSegment(at(i), a, b);
            if (squared > farthestSquared) {
                farthest = i;
                farthestSquared = squared;
            }
        }
        if (std::sqrt(farthestSquared) > tolerance) {
            keep[farthest % n] = true;
            pending.emplace_back(from, farthest);
            pending.emplace_back(farthest, to);
        }
    }
}

} // namespace

Vec2 evalQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t) {
    const double s = 1.0 - t;
    return p0 * (s * s) + p1 * (2.0 * s * t) + p2 * (t * t);
}

Vec2 evalCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t) {
    const double s = 1.0 - t;
    return p0 * (s * s * s) + p1 * (3.0 * s * s * t) + p2 * (3.0 * s * t * t) + p3 * (t * t * t);
}

std::array<Vec2, 5> splitQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t) {
    const double s = 1.0 - t;
    const Vec2 q0 = mix(p0, p1, s, t);
    const Vec2 q1 = mix(p1, p2, s, t);
    return {p0, q0, mix(q0, q1, s, t), q1, p2};
}

std::array<Vec2, 7> splitCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t) {
    const double s = 1.0 - t;
    const Vec2 q0 = mix(p0, p1, s, t);
    const Vec2 q1 = mix(p1, p2, s, t);
    const Vec2 q2 = mix(p2, p3, s, t);
    const Vec2 r0 = mix(q0, q1, s, t);
    const Vec2 r1 = mix(q1, q2, s, t);
    return {p0, q0, r0, mix(r0, r1, s, t), r1, q2, p3};
}

std::vector<Contour> flatten(const Path &path, double tolerance) {
    std::vector<Contour> contours;
    const std::vector<Vec2> &points = path.points();
    std::size_t at = 0;
    Vec2 current{};
    for (const Verb verb : path.verbs()) {
        const Vec2 *p = points.data() + at;
        switch (verb) {
        case Verb::move:
            contours.push_back({{p[0]}, false});
            current = p[0];
            break;
        case Verb::line:
            contours.back().points.push_back(p[0]);
            current = p[0];
            break;
        case Verb::quad: {
            // A quadratic's second derivative is constant: 2 (p0 - 2 p1 + p2).
            const int n = segmentsFor(2.0 * length(current - p[0] * 2.0 + p[1]), tolerance);
            for (int i = 1; i <= n; ++i)
                contours.back().points.push_back(evalQuad(current, p[0], p[1], static_cast<double>(i) / n));
            current = p[1];
            break;
        }
        case Verb::cubic: {
            // A cubic's second derivative is linear in t, so its endpoints bound it: 6 (p0 - 2 p1 + p2) at the start
            // and 6 (p1 - 2 p2 + p3) at the end.
            const double bend = 6.0 * std::max(length(current - p[0] * 2.0 + p[1]), length(p[0] - p[1] * 2.0 + p[2]));
            const int n = segmentsFor(bend, tolerance);
            for (int i = 1; i <= n; ++i)
                contours.back().points.push_back(evalCubic(current, p[0], p[1], p[2], static_cast<double>(i) / n));
            current = p[2];
            break;
        }
        case Verb::close: {
            Contour &contour = contours.back();
            contour.closed = true;
            // The closing segment is implied; a last point on top of the first would only add a zero-length edge.
            if (contour.points.size() > 1 && contour.points.back() == contour.points.front())
                contour.points.pop_back();
            current = contour.points.front();
            break;
        }
        }
        at += static_cast<std::size_t>(pointCount(verb));
    }
    return contours;
}

Contour simplified(const Contour &contour, double tolerance) {
    const std::vector<Vec2> &points = contour.points;
    const std::size_t n = points.size();
    if (n < 3)
        return contour;
    std::vector<bool> keep(n, false);
    if (contour.closed) {
        // A loop has no ends, so it is cut into two chains at two of its outermost points: corners of its convex hull,
        // as a point farthest from any other always is.
        const std::size_t a = farthestFrom(points, points.front());
        const std::size_t b = farthestFrom(points, points[a]);
        keep[a] = true;
        keep[b] = true;
        const std::size_t low = std::min(a, b);
        const std::size_t high = std::max(a, b);
        keepBetween(points, low, high, tolerance, keep);
        keepBetween(points, high, low + n, tolerance, keep);
    } else {
        keep.front() = true;
        keep.back() = true;
        keepBetween(points, 0, n - 1, tolerance, keep);
    }
    Contour result{{}, contour.closed};
    for (std::size_t i = 0; i < n; ++i)
        if (keep[i])
            result.points.push_back(points[i]);
    return result;
}

double twiceSignedArea(const Contour &contour) {
    const std::vector<Vec2> &p = contour.points;
    double area = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i)
        area += cross(p[i], p[(i + 1) % p.size()]);
    return area;
}

} // namespace linea
