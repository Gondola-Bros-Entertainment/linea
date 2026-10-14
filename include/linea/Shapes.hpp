// linea - common shapes as closed paths.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <span>

namespace linea {

// Every shape below is a closed path that winds clockwise on a y-down canvas, so shapes combined in one path add up
// under the nonzero fill rule; ring() winds its hole the other way, which cuts it out under either rule.

Path circle(Vec2 center, double radius);
// An ellipse with @p radii, rotated by @p rotation radians.
Path ellipse(Vec2 center, Vec2 radii, double rotation = 0.0);
// The rectangle between the corners @p min and @p max.
Path rectangle(Vec2 min, Vec2 max);
// A rectangle of half extents @p halfSize whose corners round with @p radius, at most the shorter half extent, rotated
// by @p rotation radians.
Path roundedRectangle(Vec2 center, Vec2 halfSize, double radius, double rotation = 0.0);
// The polygon through @p points in order, at least three of them.
Path polygon(std::span<const Vec2> points);
// A star of @p tips points, alternating between @p outer and @p inner radii, its first tip at @p rotation radians.
Path star(Vec2 center, double outer, double inner, int tips, double rotation = 0.0);
// The band between two circles about @p center, with a hole of @p inner radius.
Path ring(Vec2 center, double outer, double inner);

} // namespace linea
