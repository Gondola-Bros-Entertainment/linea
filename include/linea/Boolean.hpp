// linea - boolean operations on paths: union, intersection, difference and exclusive or.
#pragma once

#include "linea/Path.hpp"
#include "linea/Raster.hpp"

#include <cstdint>

namespace linea {

// How combine() joins the areas of two paths, named as vector editors name them.
enum class BooleanOp : std::uint8_t {
    // Inside either path: their union.
    unite,
    // Inside both paths: their intersection.
    intersect,
    // Inside the first path and outside the second: their difference.
    subtract,
    // Inside exactly one of the paths: their exclusive or.
    exclude,
};

// The area that @p op makes of the areas @p a and @p b fill under @p rule, as closed subpaths of straight lines.
//
// Each path is flattened within @p tolerance; every subpath closes, whatever its own close says, as fills close them,
// and a subpath with a point that is not finite is left out. The outlines then snap to a grid whose spacing is the
// largest power of two no more than @p tolerance / 64, or coarser where needed to keep the inputs within 2^29 steps of
// their center. Every corner of the outlines rounds to the nearest grid point, then so does every point where two of
// the rounded edges cross, and each edge bends through every such point whose cell, the square a step wide around it,
// the edge passes through, again and again until no edge passes through the cell of a point it does not end at. So
// every corner of the result lies within half a step, on both axes, of a corner of the rounded outlines or a crossing
// of their edges; edges of the result meet only at the corners they share; and every edge stays at least half a step
// away, on one axis or the other, from every corner it does not end at, so a part of the inputs thinner than that
// collapses rather than survive as a sliver or a needle.
//
// Outer contours of the result wind clockwise on a y-down canvas and holes counterclockwise inside them, and no contour
// overlaps another, so the result fills the same under either rule. No contour repeats a point, a corner lies in line
// with its neighbours only where another contour meets it, and shapes that share an edge merge along it, so the result
// strokes without seams.
Path combine(const Path &a, const Path &b, BooleanOp op, double tolerance, FillRule rule = FillRule::nonZero);

} // namespace linea
