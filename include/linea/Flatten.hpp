// linea - Bezier curves and flattening paths into polylines.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <array>
#include <vector>

namespace linea {

// The point at @p t, from 0 to 1, along a quadratic or cubic Bezier curve.
Vec2 evalQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t);
Vec2 evalCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t);

// A quadratic or cubic Bezier curve split at @p t, from 0 to 1, by de Casteljau's construction, into two curves of
// the same degree that trace it between them: points 0 to 2 (or 0 to 3) are the control points of the part from 0 to
// t, and points 2 to 4 (or 3 to 6) those of the part from t to 1. The two share their middle point, the point at t, so
// they meet exactly. Splitting at 0 or 1 gives the whole curve on one side and a single point on the other.
std::array<Vec2, 5> splitQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t);
std::array<Vec2, 7> splitCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t);

// One subpath as a polyline. A closed contour's last point joins back to its first.
struct Contour {
    std::vector<Vec2> points;
    bool closed = false;
};

// The subpaths of @p path as polylines, each curve split into straight segments that stray at most @p tolerance from
// it, in the path's units. A subpath with a single point becomes a contour of one point.
std::vector<Contour> flatten(const Path &path, double tolerance);

// @p contour with the points that the Ramer-Douglas-Peucker algorithm finds it can do without removed: every point of
// @p contour lies within @p tolerance of the result, whose points are some of @p contour's, in the same order. An
// open contour keeps both ends. A closed one has no ends, so it is simplified around its whole loop, closing edge
// included, from two of its outermost points: the one farthest from its first point, and the one farthest from that.
Contour simplified(const Contour &contour, double tolerance);

// Twice the signed area that @p contour encloses: positive when it winds clockwise on a y-down canvas.
double twiceSignedArea(const Contour &contour);

} // namespace linea
