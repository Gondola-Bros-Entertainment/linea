// linea - stroking: the area a pen of some width covers along a path.
#pragma once

#include "linea/Flatten.hpp"

#include <cstdint>
#include <vector>

namespace linea {

// How two segments of a stroke meet, as SVG's stroke-linejoin.
enum class Join : std::uint8_t { miter, round, bevel };
// How an open stroke ends, as SVG's stroke-linecap.
enum class Cap : std::uint8_t { butt, round, square };

struct StrokeStyle {
    double width = 1.0;
    Join join = Join::round;
    Cap cap = Cap::round;
    // A miter longer than this many stroke widths becomes a bevel, as SVG's stroke-miterlimit.
    double miterLimit = 4.0;
};

// The area that a pen of @p style covers along @p contours, as polygons that fill it under the nonzero rule. Round
// joins and caps stray at most @p tolerance from true arcs.
std::vector<Contour> strokeOutline(const std::vector<Contour> &contours, const StrokeStyle &style, double tolerance);

} // namespace linea
