// linea - boolean operations on contours already flattened, which combine() and strokes share. Internal.
#pragma once

#include "linea/Boolean.hpp"
#include "linea/Flatten.hpp"
#include "linea/Raster.hpp"

#include <vector>

namespace linea::detail {

// The area that @p op makes of the areas @p a and @p b fill under @p rule, as combine() makes it of paths flattened to
// these contours: closed contours of straight lines that do not overlap, outer ones clockwise on a y-down canvas and
// holes counterclockwise, snapped to the grid that @p tolerance sets. Every contour closes, and one with a point that
// is not finite is left out.
std::vector<Contour> combineContours(std::vector<Contour> a, std::vector<Contour> b, BooleanOp op, double tolerance,
                                     FillRule rule);

} // namespace linea::detail
