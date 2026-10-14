// linea - measuring paths: their length, the point and direction at a distance along them, and cutting them there.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <optional>

namespace linea {

// The length of @p path: the sum of its segments' lengths, the line that closes each closed subpath included. A line
// measures exactly, as the distance between its ends. A quadratic or cubic curve measures by Gauss-Legendre quadrature
// of its speed: the curve is cut where its speed is least, so that a cusp ends a piece rather than hiding inside one,
// and each piece is halved, and its halves halved, until two estimates agree within 1e-12 of the length of the curve's
// control polygon, which bounds the curve's own length.
double arcLength(const Path &path);

// A place on a path, and the way the path heads there.
struct PathPoint {
    Vec2 point{};
    // The unit vector along the path's direction of travel.
    Vec2 tangent{};
};

// Where @p path is after @p distance along it, measured as arcLength() measures, from the start of its first subpath
// through each subpath in turn. Nothing when @p distance is negative or more than the path's length, or the path has no
// length; segments of no length are passed over. At 0 the point is exactly where the path starts to have length, and at
// the path's length exactly where its last segment of any length ends. At a corner, the point and tangent are those of
// the segment arriving there. On a curve, the point is where the curve's length from its start, as arcLength() measures
// it, matches the distance within 1e-12 of the length of the curve's control polygon. Where a curve stops for an
// instant, as one whose control point sits on its end does, the tangent is the way it moves just before, or just after
// at its start.
std::optional<PathPoint> pointAt(const Path &path, double distance);

// The two parts of a path on either side of a cut.
struct PathParts {
    Path before{};
    Path after{};
};

// @p path cut at @p distance along it, measured as arcLength() measures, into the part before and the part after, which
// together trace the whole path. A curve is cut exactly, split at the parameter where it has run the distance, and the
// two parts meet at the point that pointAt() gives. A cut where a subpath's length ends falls between that subpath and
// the next; one within a subpath ends the part before there and starts the part after there, and a closed subpath cut
// in two is open in both, the part after running on around to the subpath's start. A distance of 0 or less, or one
// that is not a number, leaves the whole path after the cut, and one of the path's length or more leaves it before.
PathParts splitAt(const Path &path, double distance);

} // namespace linea
