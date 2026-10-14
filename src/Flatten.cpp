// linea - Flatten implementation.
#include "linea/Flatten.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

} // namespace

Vec2 evalQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t) {
    const double s = 1.0 - t;
    return p0 * (s * s) + p1 * (2.0 * s * t) + p2 * (t * t);
}

Vec2 evalCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t) {
    const double s = 1.0 - t;
    return p0 * (s * s * s) + p1 * (3.0 * s * s * t) + p2 * (3.0 * s * t * t) + p3 * (t * t * t);
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

double twiceSignedArea(const Contour &contour) {
    const std::vector<Vec2> &p = contour.points;
    double area = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i)
        area += cross(p[i], p[(i + 1) % p.size()]);
    return area;
}

} // namespace linea
