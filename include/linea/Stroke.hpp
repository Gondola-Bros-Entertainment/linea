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
    // The lengths of dashes and of the gaps between them, alternating from a dash, as SVG's stroke-dasharray: an odd
    // count repeats once to make an even one. An empty array strokes solid, as does one that sums to zero, one with a
    // negative length, which SVG holds invalid, and one with a length that is not finite.
    std::vector<double> dashArray{};
    // How far into the dash pattern each subpath starts, as SVG's stroke-dashoffset; a negative offset starts the
    // pattern that far into the subpath. An offset that is not finite strokes solid.
    double dashOffset = 0.0;
};

// A dash pattern that would cut a stroke's contours into more than this many dashes in all strokes them solid instead,
// as Skia gives up past a limit of its own, so that a pattern far finer than its path cannot exhaust memory.
inline constexpr double mostDashes = 65536.0;

// The area that a pen of @p style covers along @p contours, as polygons that fill it under the nonzero rule. Round
// joins and caps stray at most @p tolerance from true arcs.
//
// A dash pattern cuts each contour into dashes at SVG's dash positions, measured along the contour from its first
// point. Each dash is stroked on its own, with a cap at each end and a join at each corner within it. On a closed
// contour, a dash that runs on through the first point joins there, as Skia, and so Chrome, draws it, where SVG's
// algorithm alone would leave two caps; a dash all the way around is the contour's solid stroke.
std::vector<Contour> strokeOutline(const std::vector<Contour> &contours, const StrokeStyle &style, double tolerance);

} // namespace linea
