// linea - Bezier curves and flattening paths into polylines.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <vector>

namespace linea {

// The point at @p t, from 0 to 1, along a quadratic or cubic Bezier curve.
Vec2 evalQuad(Vec2 p0, Vec2 p1, Vec2 p2, double t);
Vec2 evalCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t);

// One subpath as a polyline. A closed contour's last point joins back to its first.
struct Contour {
    std::vector<Vec2> points;
    bool closed = false;
};

// The subpaths of @p path as polylines, each curve split into straight segments that stray at most @p tolerance from
// it, in the path's units. A subpath with a single point becomes a contour of one point.
std::vector<Contour> flatten(const Path &path, double tolerance);

// Twice the signed area that @p contour encloses: positive when it winds clockwise on a y-down canvas.
double twiceSignedArea(const Contour &contour);

} // namespace linea
