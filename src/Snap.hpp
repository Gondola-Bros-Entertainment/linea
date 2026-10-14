// linea - snap rounding: edges on an integer grid, bent so that they meet only at grid points. Internal.
#pragma once

#include <compare>
#include <cstdint>
#include <vector>

namespace linea::detail {

// A point of the integer grid that boolean operations round coordinates to.
struct GridPoint {
    std::int64_t x{};
    std::int64_t y{};
    friend constexpr bool operator==(GridPoint, GridPoint) = default;
    // By x, then y: the order in which a sweep from left to right meets points.
    friend constexpr auto operator<=>(GridPoint, GridPoint) = default;
};

// Every coordinate lies within this distance of zero, so that the difference of any two is below 2^30 and every cross
// product of differences, below 2^61, is exact in 64-bit integers.
inline constexpr std::int64_t gridReach = std::int64_t{1} << 29;

// Twice the signed area of the triangle @p o, @p a, @p b: positive when @p b lies on the positive side of the direction
// from @p o to @p a, its right on a y-down canvas. Exact, since gridReach bounds every coordinate.
constexpr std::int64_t cross(GridPoint o, GridPoint a, GridPoint b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// A directed edge of one of two outlines, operand 0 or 1.
struct GridEdge {
    GridPoint from;
    GridPoint to;
    int operand = 0;
};

// @p edges snap-rounded. Every corner of an edge and every point where two edges cross makes the grid cell around it
// hot: the square of side one centered on the nearest grid point, closed on its low sides and open on its high ones.
// Each edge then bends through the center of every hot cell it passes through, and each of its pieces again, until no
// piece passes through a hot cell it does not end in. The pieces come back directed and labeled as the edges they came
// from. Any two pieces are equal or meet at most at an end, and every grid point that ends a piece lies at least half a
// step, on one axis or the other, from every piece it does not end.
std::vector<GridEdge> snapRound(const std::vector<GridEdge> &edges);

} // namespace linea::detail
