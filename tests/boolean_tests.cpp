#include "linea/Boolean.hpp"
#include "linea/Flatten.hpp"
#include "linea/Raster.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

using namespace linea;

namespace {

constexpr BooleanOp operations[] = {BooleanOp::unite, BooleanOp::intersect, BooleanOp::subtract, BooleanOp::exclude};
constexpr FillRule rules[] = {FillRule::nonZero, FillRule::evenOdd};

// Combined paths hold only straight lines, which flatten to their own points at any tolerance.
constexpr double linesOnly = 1.0;
// Fine enough that rounding to the grid, a power of two at most 1/64 of it, shows only far below these tests' units.
constexpr double tolerance = 1e-3;
// Coarse enough that the grid is one unit, so that snapping moves every crossing of integer corners by up to half of
// one.
constexpr double unitGridTolerance = 64.0;

std::vector<Contour> contoursOf(const Path &path) { return flatten(path, linesOnly); }

double area(const std::vector<Contour> &contours) {
    double twice = 0.0;
    for (const Contour &contour : contours)
        twice += twiceSignedArea(contour);
    return twice / 2.0;
}

double area(const Path &path) { return area(contoursOf(path)); }

double perimeter(const std::vector<Contour> &contours) {
    double sum = 0.0;
    for (const Contour &contour : contours)
        for (std::size_t i = 0; i < contour.points.size(); ++i)
            sum += length(contour.points[(i + 1) % contour.points.size()] - contour.points[i]);
    return sum;
}

// The spacing of the grid that combine() rounds to at tolerance @p tol, for inputs as small as these tests'.
double gridSpacing(double tol) { return std::ldexp(1.0, std::ilogb(tol / 64.0)); }

// How far snapping to the grid can move the area of one result: its edges are the inputs' edges bent through grid
// points near them, no longer in all than the inputs' perimeter, and bent less than a step away from them on average.
double roundingBudget(const Path &a, const Path &b, double tol = tolerance) {
    return (perimeter(flatten(a, tol)) + perimeter(flatten(b, tol))) * gridSpacing(tol);
}

int winding(Vec2 p, const std::vector<Contour> &contours) {
    int count = 0;
    for (const Contour &contour : contours) {
        for (std::size_t i = 0; i < contour.points.size(); ++i) {
            const Vec2 from = contour.points[i];
            const Vec2 to = contour.points[(i + 1) % contour.points.size()];
            const double side = cross(to - from, p - from);
            if (from.y <= p.y && to.y > p.y && side > 0.0)
                ++count;
            else if (to.y <= p.y && from.y > p.y && side < 0.0)
                --count;
        }
    }
    return count;
}

double distanceToEdges(Vec2 p, const std::vector<Contour> &contours) {
    double nearest = std::numeric_limits<double>::infinity();
    for (const Contour &contour : contours) {
        for (std::size_t i = 0; i < contour.points.size(); ++i) {
            const Vec2 from = contour.points[i];
            const Vec2 edge = contour.points[(i + 1) % contour.points.size()] - from;
            const double t = std::clamp(dot(p - from, edge) / dot(edge, edge), 0.0, 1.0);
            nearest = std::min(nearest, length(p - (from + edge * t)));
        }
    }
    return nearest;
}

bool fills(int count, FillRule rule) { return rule == FillRule::nonZero ? count != 0 : count % 2 != 0; }

bool combined(BooleanOp op, bool a, bool b) {
    switch (op) {
    case BooleanOp::unite:
        return a || b;
    case BooleanOp::intersect:
        return a && b;
    case BooleanOp::subtract:
        return a && !b;
    case BooleanOp::exclude:
        return a != b;
    }
    return false;
}

// The area where @p op combines what @p a and @p b fill under @p rule, found without boolean operations: between
// every two heights where an edge starts, ends or crosses another, edges keep their order, so the filled width is
// linear in y and its value halfway up times the band's height is the band's area.
double integratedArea(const std::vector<Contour> &a, const std::vector<Contour> &b, BooleanOp op, FillRule rule) {
    struct Edge {
        Vec2 from;
        Vec2 to;
        int operand;
    };
    std::vector<Edge> edges;
    std::vector<double> heights;
    for (const auto *contours : {&a, &b}) {
        for (const Contour &contour : *contours) {
            for (std::size_t i = 0; i < contour.points.size(); ++i) {
                const Vec2 from = contour.points[i];
                const Vec2 to = contour.points[(i + 1) % contour.points.size()];
                heights.push_back(from.y);
                if (from.y != to.y)
                    edges.push_back({from, to, contours == &a ? 0 : 1});
            }
        }
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        for (std::size_t j = i + 1; j < edges.size(); ++j) {
            const Vec2 d1 = edges[i].to - edges[i].from;
            const Vec2 d2 = edges[j].to - edges[j].from;
            const double det = cross(d1, d2);
            if (det == 0.0)
                continue;
            const double t = cross(edges[j].from - edges[i].from, d2) / det;
            const double u = cross(edges[j].from - edges[i].from, d1) / det;
            if (t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0)
                heights.push_back(edges[i].from.y + t * d1.y);
        }
    }
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());

    double total = 0.0;
    for (std::size_t k = 0; k + 1 < heights.size(); ++k) {
        const double middle = (heights[k] + heights[k + 1]) / 2.0;
        std::vector<std::pair<double, const Edge *>> crossings;
        for (const Edge &edge : edges)
            if (std::min(edge.from.y, edge.to.y) < middle && middle < std::max(edge.from.y, edge.to.y))
                crossings.emplace_back(edge.from.x + (middle - edge.from.y) * (edge.to.x - edge.from.x) /
                                                         (edge.to.y - edge.from.y),
                                       &edge);
        std::sort(crossings.begin(), crossings.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });
        int windings[2] = {0, 0};
        for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
            const Edge &edge = *crossings[i].second;
            windings[edge.operand] += edge.to.y < edge.from.y ? 1 : -1;
            if (combined(op, fills(windings[0], rule), fills(windings[1], rule)))
                total += (crossings[i + 1].first - crossings[i].first) * (heights[k + 1] - heights[k]);
        }
    }
    return total;
}

// How many points of a lattice four to a unit over the square from 0 to @p extent lie on the wrong side of @p result:
// in it where @p op leaves them out, or out of it where @p op puts them in. Only points three grid steps at @p tol
// clear of every edge of @p a and @p b count: snapping moves a corner half a step, and each further bend half a step
// more, which on random shapes like these has stayed within two steps.
int misplacedPoints(const Path &a, const Path &b, BooleanOp op, FillRule rule, double tol, const Path &result,
                    double extent) {
    constexpr double pointsPerUnit = 4.0;
    const int side = static_cast<int>(std::ceil(extent * pointsPerUnit));
    constexpr double clearSteps = 3.0;
    const std::vector<Contour> first = flatten(a, tol);
    const std::vector<Contour> second = flatten(b, tol);
    const std::vector<Contour> outline = contoursOf(result);
    const double clearance = clearSteps * gridSpacing(tol);
    int misplaced = 0;
    for (int i = 0; i < side; ++i) {
        for (int j = 0; j < side; ++j) {
            const Vec2 p{(i + 0.5) * extent / side, (j + 0.5) * extent / side};
            if (distanceToEdges(p, first) < clearance || distanceToEdges(p, second) < clearance)
                continue;
            const bool expected = combined(op, fills(winding(p, first), rule), fills(winding(p, second), rule));
            if (winding(p, outline) != (expected ? 1 : 0))
                ++misplaced;
        }
    }
    return misplaced;
}

// Whether the segment from @p p to @p q passes within @p reach of @p c on both axes at once.
bool passesWithin(Vec2 p, Vec2 q, Vec2 c, double reach) {
    double first = 0.0;
    double last = 1.0;
    for (const auto [from, along, centre] :
         {std::array<double, 3>{p.x, q.x - p.x, c.x}, std::array<double, 3>{p.y, q.y - p.y, c.y}}) {
        if (along == 0.0) {
            if (std::abs(from - centre) >= reach)
                return false;
            continue;
        }
        const double enter = (centre - reach - from) / along;
        const double leave = (centre + reach - from) / along;
        first = std::max(first, std::min(enter, leave));
        last = std::min(last, std::max(enter, leave));
    }
    return first < last;
}

// Whether no contour of @p contours repeats a point, a corner lies in line with its neighbours only where another
// contour meets it, and every edge stays at least half a grid step at tolerance @p tol away, on one axis or the other,
// from every corner it does not end at.
bool cornersStandApart(const std::vector<Contour> &contours, double tol) {
    // Corners and edges' ends lie on the grid, so only rounding in the clipping can blur an edge exactly half a step
    // away; this slack allows for it.
    constexpr double slack = 1e-9;
    const double reach = gridSpacing(tol) / 2.0 * (1.0 - slack);
    const auto contoursThrough = [&](Vec2 corner) {
        return std::count_if(contours.begin(), contours.end(), [&](const Contour &contour) {
            return std::find(contour.points.begin(), contour.points.end(), corner) != contour.points.end();
        });
    };
    for (const Contour &contour : contours) {
        const std::vector<Vec2> &p = contour.points;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (std::find(p.begin() + static_cast<std::ptrdiff_t>(i) + 1, p.end(), p[i]) != p.end())
                return false;
            const Vec2 before = p[(i + p.size() - 1) % p.size()];
            const Vec2 after = p[(i + 1) % p.size()];
            if (cross(p[i] - before, after - p[i]) == 0.0 && contoursThrough(p[i]) < 2)
                return false;
        }
    }
    for (const Contour &corners : contours)
        for (const Vec2 corner : corners.points)
            for (const Contour &edges : contours)
                for (std::size_t i = 0; i < edges.points.size(); ++i) {
                    const Vec2 from = edges.points[i];
                    const Vec2 to = edges.points[(i + 1) % edges.points.size()];
                    if (corner != from && corner != to && passesWithin(from, to, corner, reach))
                        return false;
                }
    return true;
}

// Whether no two edges of @p contours meet anywhere but at an end they share, nor run along each other.
bool edgesMeetOnlyAtCorners(const std::vector<Contour> &contours) {
    std::vector<std::pair<Vec2, Vec2>> edges;
    for (const Contour &contour : contours)
        for (std::size_t i = 0; i < contour.points.size(); ++i)
            edges.emplace_back(contour.points[i], contour.points[(i + 1) % contour.points.size()]);
    // Corners lie on a grid of powers of two, so these cross products are exact.
    const auto onEdge = [](Vec2 p, Vec2 from, Vec2 to) {
        return cross(to - from, p - from) == 0.0 && std::min(from.x, to.x) <= p.x && p.x <= std::max(from.x, to.x) &&
               std::min(from.y, to.y) <= p.y && p.y <= std::max(from.y, to.y);
    };
    for (std::size_t i = 0; i < edges.size(); ++i) {
        for (std::size_t j = i + 1; j < edges.size(); ++j) {
            const auto [p, q] = edges[i];
            const auto [r, s] = edges[j];
            if ((p == r && q == s) || (p == s && q == r))
                return false;
            const double sideR = cross(q - p, r - p);
            const double sideS = cross(q - p, s - p);
            const double sideP = cross(s - r, p - r);
            const double sideQ = cross(s - r, q - r);
            if (((sideR < 0.0 && sideS > 0.0) || (sideR > 0.0 && sideS < 0.0)) &&
                ((sideP < 0.0 && sideQ > 0.0) || (sideP > 0.0 && sideQ < 0.0)))
                return false;
            for (const Vec2 end : {r, s})
                if (end != p && end != q && onEdge(end, p, q))
                    return false;
            for (const Vec2 end : {p, q})
                if (end != r && end != s && onEdge(end, r, s))
                    return false;
        }
    }
    return true;
}

// Whether every corner of @p contours lies within half a grid step at tolerance @p tol, on both axes, of a corner of
// @p a or @p b or of a point where two of their edges cross. Their corners must lie on the grid already.
bool cornersNearInputs(const Path &a, const Path &b, const std::vector<Contour> &contours, double tol) {
    std::vector<Vec2> exact;
    std::vector<std::pair<Vec2, Vec2>> edges;
    for (const Path *path : {&a, &b}) {
        for (const Contour &contour : flatten(*path, tol)) {
            for (std::size_t i = 0; i < contour.points.size(); ++i) {
                exact.push_back(contour.points[i]);
                edges.emplace_back(contour.points[i], contour.points[(i + 1) % contour.points.size()]);
            }
        }
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        for (std::size_t j = i + 1; j < edges.size(); ++j) {
            const Vec2 d = edges[i].second - edges[i].first;
            const Vec2 e = edges[j].second - edges[j].first;
            const double det = cross(d, e);
            if (det == 0.0)
                continue;
            const double t = cross(edges[j].first - edges[i].first, e) / det;
            const double u = cross(edges[j].first - edges[i].first, d) / det;
            if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0)
                exact.push_back(edges[i].first + d * t);
        }
    }
    // Crossings computed in floating point miss their exact place by far less than this share of a step.
    constexpr double slack = 1e-9;
    const double reach = gridSpacing(tol) / 2.0 * (1.0 + slack);
    for (const Contour &contour : contours) {
        for (const Vec2 corner : contour.points) {
            if (std::none_of(exact.begin(), exact.end(), [&](Vec2 p) {
                    return std::abs(p.x - corner.x) <= reach && std::abs(p.y - corner.y) <= reach;
                }))
                return false;
        }
    }
    return true;
}

// A path of @p count subpaths with integer corners in [0, @p extent): polygons through random points, which cross
// themselves and each other, and rectangles, which share edges and corners. Only the engine's raw output is used, which
// the standard fixes, so every platform draws the same shapes.
Path randomShapes(std::mt19937 &rng, int count, std::uint32_t extent) {
    constexpr std::uint32_t fewestCorners = 3;
    constexpr std::uint32_t moreCorners = 8;
    const auto coordinate = [&] { return static_cast<double>(rng() % extent); };
    Path path;
    for (int i = 0; i < count; ++i) {
        if (rng() % 3 == 0) {
            path.append(rectangle({coordinate(), coordinate()}, {coordinate(), coordinate()}));
            continue;
        }
        std::vector<Vec2> corners(fewestCorners + rng() % moreCorners);
        for (Vec2 &corner : corners)
            corner = {coordinate(), coordinate()};
        path.append(polygon(corners));
    }
    return path;
}

// Whether @p contour visits exactly @p expected, starting anywhere.
bool sameCycle(const Contour &contour, const std::vector<Vec2> &expected) {
    const std::vector<Vec2> &points = contour.points;
    if (points.size() != expected.size())
        return false;
    const auto start = std::find(points.begin(), points.end(), expected.front());
    if (start == points.end())
        return false;
    std::vector<Vec2> rotated(start, points.end());
    rotated.insert(rotated.end(), points.begin(), start);
    return rotated == expected;
}

} // namespace

TEST_CASE("Each operation on two overlapping squares gives exactly the outline it names") {
    const Path a = rectangle({0.0, 0.0}, {4.0, 4.0});
    const Path b = rectangle({2.0, 2.0}, {6.0, 6.0});
    const std::vector<Contour> both = contoursOf(combine(a, b, BooleanOp::unite, tolerance));
    REQUIRE(both.size() == 1);
    CHECK(sameCycle(both[0],
                    {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {6.0, 2.0}, {6.0, 6.0}, {2.0, 6.0}, {2.0, 4.0}, {0.0, 4.0}}));
    const std::vector<Contour> shared = contoursOf(combine(a, b, BooleanOp::intersect, tolerance));
    REQUIRE(shared.size() == 1);
    CHECK(sameCycle(shared[0], {{2.0, 2.0}, {4.0, 2.0}, {4.0, 4.0}, {2.0, 4.0}}));
    const std::vector<Contour> rest = contoursOf(combine(a, b, BooleanOp::subtract, tolerance));
    REQUIRE(rest.size() == 1);
    CHECK(sameCycle(rest[0], {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {2.0, 2.0}, {2.0, 4.0}, {0.0, 4.0}}));
    // The two L shapes of the exclusive or touch only at corners, so they stay apart.
    const std::vector<Contour> either = contoursOf(combine(a, b, BooleanOp::exclude, tolerance));
    CHECK(either.size() == 2);
    CHECK(area(either) == 24.0);
}

TEST_CASE("Every operation covers the area that integrating both inputs' winding numbers gives" *
          doctest::test_suite("slow")) {
    // Integer corners put many crossings, shared edges and shared corners exactly on top of one another.
    std::mt19937 rng(20240611);
    constexpr int trials = 120;
    constexpr std::uint32_t extent = 16;
    for (int trial = 0; trial < trials; ++trial) {
        const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const double budget = roundingBudget(a, b);
        for (const FillRule rule : rules) {
            for (const BooleanOp op : operations) {
                const double expected = integratedArea(flatten(a, tolerance), flatten(b, tolerance), op, rule);
                CHECK(std::abs(area(combine(a, b, op, tolerance, rule)) - expected) <= budget);
            }
        }
    }
}

TEST_CASE("Union and intersection add up to both shapes, and difference and exclusive or follow from them" *
          doctest::test_suite("slow")) {
    std::mt19937 rng(7);
    constexpr int trials = 120;
    constexpr std::uint32_t extent = 16;
    for (int trial = 0; trial < trials; ++trial) {
        const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const double budget = roundingBudget(a, b);
        for (const FillRule rule : rules) {
            const double areaA = area(combine(a, Path{}, BooleanOp::unite, tolerance, rule));
            const double areaB = area(combine(b, Path{}, BooleanOp::unite, tolerance, rule));
            const double both = area(combine(a, b, BooleanOp::unite, tolerance, rule));
            const double shared = area(combine(a, b, BooleanOp::intersect, tolerance, rule));
            // Each side sums areas that each round within the budget.
            CHECK(std::abs(both + shared - (areaA + areaB)) <= 4.0 * budget);
            CHECK(std::abs(area(combine(a, b, BooleanOp::subtract, tolerance, rule)) - (areaA - shared)) <=
                  3.0 * budget);
            CHECK(std::abs(area(combine(a, b, BooleanOp::exclude, tolerance, rule)) - (both - shared)) <= 3.0 * budget);
        }
    }
}

TEST_CASE("Every point clear of both inputs' edges lies in the result exactly when the operation puts it there" *
          doctest::test_suite("slow")) {
    std::mt19937 rng(31337);
    constexpr int trials = 40;
    constexpr std::uint32_t extent = 16;
    for (int trial = 0; trial < trials; ++trial) {
        const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        for (const FillRule rule : rules) {
            for (const BooleanOp op : operations) {
                const Path result = combine(a, b, op, tolerance, rule);
                CHECK(misplacedPoints(a, b, op, rule, tolerance, result, extent) == 0);
                CHECK(cornersStandApart(contoursOf(result), tolerance));
            }
        }
    }
}

TEST_CASE("Edges of the result meet only at the corners they share, and keep half a step from all others" *
          doctest::test_suite("slow")) {
    std::mt19937 rng(2718);
    constexpr int trials = 40;
    constexpr std::uint32_t extent = 16;
    for (const double tol : {tolerance, unitGridTolerance}) {
        for (int trial = 0; trial < trials; ++trial) {
            const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
            const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
            for (const FillRule rule : rules) {
                for (const BooleanOp op : operations) {
                    const std::vector<Contour> result = contoursOf(combine(a, b, op, tol, rule));
                    CHECK(edgesMeetOnlyAtCorners(result));
                    CHECK(cornersStandApart(result, tol));
                }
            }
        }
    }
}

TEST_CASE("Every corner of the result lies within half a step of a corner or crossing of the inputs" *
          doctest::test_suite("slow")) {
    std::mt19937 rng(1618);
    constexpr int trials = 40;
    constexpr std::uint32_t extent = 16;
    for (const double tol : {tolerance, unitGridTolerance}) {
        for (int trial = 0; trial < trials; ++trial) {
            const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
            const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
            for (const FillRule rule : rules)
                for (const BooleanOp op : operations)
                    CHECK(cornersNearInputs(a, b, contoursOf(combine(a, b, op, tol, rule)), tol));
        }
    }
}

TEST_CASE("Polygons that share corners, overlap along edges and cross themselves fill as their winding numbers say" *
          doctest::test_suite("slow")) {
    // Corners drawn from a lattice six units wide repeat, line up and overlap often.
    std::mt19937 rng(4242);
    constexpr int trials = 150;
    constexpr std::uint32_t extent = 6;
    for (int trial = 0; trial < trials; ++trial) {
        const Path a = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const Path b = randomShapes(rng, 1 + static_cast<int>(rng() % 3), extent);
        const double budget = roundingBudget(a, b);
        for (const FillRule rule : rules) {
            for (const BooleanOp op : operations) {
                const Path result = combine(a, b, op, tolerance, rule);
                CHECK(misplacedPoints(a, b, op, rule, tolerance, result, extent) == 0);
                const double expected = integratedArea(flatten(a, tolerance), flatten(b, tolerance), op, rule);
                CHECK(std::abs(area(result) - expected) <= budget);
            }
        }
    }
}

TEST_CASE("A hole that touches the outline at one corner stays a hole") {
    // The square's corner (3, 8) lies on the long edge from (1, 11) to (5, 5), and the union leaves a triangular hole
    // between the square, the triangle and that edge, touching the outline at the corner. On a grid of 2^-20, which
    // this tolerance gives, the crossing at the hole's far corner rounds to within a step of the edge.
    constexpr double fineTolerance = 1.0 / 16384.0;
    const Vec2 triangle[] = {{4.0, 9.0}, {2.0, 1.0}, {6.0, 3.0}};
    const Vec2 square[] = {{4.0, 8.0}, {3.0, 8.0}, {3.0, 9.0}, {4.0, 9.0}};
    const Vec2 bow[] = {{0.0, 8.0}, {4.0, 1.0}, {3.0, 3.0}, {1.0, 6.0}, {1.0, 11.0}, {5.0, 5.0}};
    const Path a = polygon(triangle);
    Path b = polygon(square);
    b.append(polygon(bow));
    const Path both = combine(a, b, BooleanOp::unite, fineTolerance);
    const double expected =
        integratedArea(flatten(a, fineTolerance), flatten(b, fineTolerance), BooleanOp::unite, FillRule::nonZero);
    CHECK(std::abs(area(both) - expected) <= roundingBudget(a, b, fineTolerance));
    CHECK(misplacedPoints(a, b, BooleanOp::unite, FillRule::nonZero, fineTolerance, both, 12.0) == 0);
}

TEST_CASE("Edges that only meet at a corner never join as if they ran together") {
    // Two edges of the second shape rise to the corner (3, 4), crossed just below it by two edges of the first. On a
    // grid of 1/64, which this tolerance gives, both crossings round to points within a step of either edge.
    constexpr double coarseTolerance = 1.0;
    const Vec2 a1[] = {{8.0, 7.0}, {1.0, 2.0}, {1.0, 10.0}, {2.0, 4.0}, {7.0, 3.0}, {8.0, 9.0}, {7.0, 10.0}};
    const Vec2 a2[] = {{11.0, 7.0}, {0.0, 3.0}, {1.0, 7.0},  {8.0, 9.0}, {9.0, 2.0},
                       {9.0, 8.0},  {7.0, 5.0}, {3.0, 10.0}, {8.0, 7.0}};
    const Vec2 a3[] = {{11.0, 8.0}, {10.0, 8.0}, {11.0, 9.0}, {11.0, 7.0}, {1.0, 2.0}, {9.0, 11.0}};
    const Vec2 b1[] = {{3.0, 5.0}, {9.0, 3.0}, {9.0, 0.0}, {3.0, 0.0}, {8.0, 3.0}, {3.0, 2.0}};
    const Vec2 b2[] = {{0.0, 0.0}, {9.0, 1.0}, {8.0, 11.0}, {5.0, 4.0}, {1.0, 11.0}, {3.0, 4.0}, {1.0, 8.0}};
    Path a = polygon(a1);
    a.append(polygon(a2)).append(polygon(a3));
    Path b = polygon(b1);
    b.append(polygon(b2));
    const Path shared = combine(a, b, BooleanOp::intersect, coarseTolerance);
    CHECK(misplacedPoints(a, b, BooleanOp::intersect, FillRule::nonZero, coarseTolerance, shared, 12.0) == 0);
}

TEST_CASE("An outline that runs back along itself leaves no needle") {
    // The exclusive or's outline runs to (3, 7) and straight back. On a grid of 2^-20, which this tolerance gives, a
    // crossing there rounds a step off the line, which could leave the two runs a step apart: a needle with no area.
    constexpr double fineTolerance = 1.0 / 16384.0;
    const Vec2 first[] = {{8.0, 3.0}, {3.0, 7.0}, {3.0, 4.0}};
    const Vec2 second[] = {{6.0, 4.0},  {0.0, 8.0}, {9.0, 5.0}, {7.0, 9.0}, {7.0, 6.0},
                           {8.0, 10.0}, {3.0, 4.0}, {2.0, 4.0}, {5.0, 10.0}};
    const Path a = polygon(first);
    const Path b = polygon(second);
    const Path either = combine(a, b, BooleanOp::exclude, fineTolerance, FillRule::evenOdd);
    CHECK(cornersStandApart(contoursOf(either), fineTolerance));
    CHECK(misplacedPoints(a, b, BooleanOp::exclude, FillRule::evenOdd, fineTolerance, either, 12.0) == 0);
}

TEST_CASE("A circle minus an overlapping circle is one crescent of the analytic area, without slivers") {
    constexpr double r = 10.0;
    constexpr double d = 8.0;
    const Vec2 centerA{20.0, 20.0};
    const Vec2 centerB{20.0 + d, 20.0};
    const std::vector<Contour> crescent =
        contoursOf(combine(circle(centerA, r), circle(centerB, r), BooleanOp::subtract, tolerance));
    REQUIRE(crescent.size() == 1);
    // The lens two circles of radius r share at distance d, taken from one.
    const double lens = 2.0 * r * r * std::acos(d / (2.0 * r)) - d / 2.0 * std::sqrt(4.0 * r * r - d * d);
    CHECK(area(crescent) == doctest::Approx(std::numbers::pi * r * r - lens).epsilon(1e-3));
    // The crescent's outline is the arc of one circle outside the other and of the other inside the first, which for
    // equal circles measure one whole circumference together: a sliver would add length without area.
    CHECK(perimeter(crescent) == doctest::Approx(2.0 * std::numbers::pi * r).epsilon(1e-3));
    // Arcs become cubics within 0.03% of the radius, and those become lines within the tolerance.
    constexpr double onCircle = 0.005;
    for (const Vec2 p : crescent.front().points) {
        const double fromA = length(p - centerA);
        const double fromB = length(p - centerB);
        CHECK((std::abs(fromA - r) <= onCircle || std::abs(fromB - r) <= onCircle));
        CHECK(fromA <= r + onCircle);
        CHECK(fromB >= r - onCircle);
    }
}

TEST_CASE("A shape combined with itself keeps its outline under union and intersection and vanishes otherwise") {
    for (const Path &shape : {star({20.0, 20.0}, 15.0, 6.0, 5), circle({20.0, 20.0}, 12.0)}) {
        const std::vector<Contour> outline = flatten(shape, tolerance);
        for (const BooleanOp op : {BooleanOp::unite, BooleanOp::intersect}) {
            const std::vector<Contour> result = contoursOf(combine(shape, shape, op, tolerance));
            REQUIRE(result.size() == 1);
            CHECK(result.front().points.size() == outline.front().points.size());
            CHECK(std::abs(area(result) - area(outline)) <= roundingBudget(shape, Path{}));
        }
        CHECK(combine(shape, shape, BooleanOp::subtract, tolerance).empty());
        CHECK(combine(shape, shape, BooleanOp::exclude, tolerance).empty());
    }
}

TEST_CASE("Shapes that share an edge or part of one merge into one outline") {
    const Path left = rectangle({0.0, 0.0}, {2.0, 2.0});
    const std::vector<Contour> side =
        contoursOf(combine(left, rectangle({2.0, 0.0}, {4.0, 2.0}), BooleanOp::unite, tolerance));
    REQUIRE(side.size() == 1);
    CHECK(sameCycle(side[0], {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {0.0, 2.0}}));
    const std::vector<Contour> stacked =
        contoursOf(combine(left, rectangle({0.0, 2.0}, {2.0, 4.0}), BooleanOp::unite, tolerance));
    REQUIRE(stacked.size() == 1);
    CHECK(sameCycle(stacked[0], {{0.0, 0.0}, {2.0, 0.0}, {2.0, 4.0}, {0.0, 4.0}}));
    // Collinear edges that overlap only in part.
    const std::vector<Contour> tee = contoursOf(
        combine(rectangle({0.0, 0.0}, {4.0, 2.0}), rectangle({1.0, 2.0}, {3.0, 5.0}), BooleanOp::unite, tolerance));
    REQUIRE(tee.size() == 1);
    CHECK(sameCycle(tee[0],
                    {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {3.0, 2.0}, {3.0, 5.0}, {1.0, 5.0}, {1.0, 2.0}, {0.0, 2.0}}));
    // Touching along an edge shares no area.
    CHECK(combine(left, rectangle({2.0, 0.0}, {4.0, 2.0}), BooleanOp::intersect, tolerance).empty());
}

TEST_CASE("Shapes that touch only at a corner stay separate outlines") {
    const Path a = rectangle({0.0, 0.0}, {2.0, 2.0});
    const Path b = rectangle({2.0, 2.0}, {4.0, 4.0});
    const std::vector<Contour> both = contoursOf(combine(a, b, BooleanOp::unite, tolerance));
    CHECK(both.size() == 2);
    CHECK(area(both) == 8.0);
    CHECK(combine(a, b, BooleanOp::intersect, tolerance).empty());
}

TEST_CASE("Concave, nested and self-intersecting inputs fill under the rule they are given") {
    // A C opening to the right, and a bar through its opening.
    const Vec2 cCorners[] = {{0.0, 0.0}, {6.0, 0.0}, {6.0, 2.0}, {2.0, 2.0},
                             {2.0, 4.0}, {6.0, 4.0}, {6.0, 6.0}, {0.0, 6.0}};
    const Path c = polygon(cCorners);
    const Path bar = rectangle({1.0, 1.0}, {8.0, 5.0});
    CHECK(area(combine(c, bar, BooleanOp::unite, tolerance)) == 44.0);
    CHECK(area(combine(c, bar, BooleanOp::intersect, tolerance)) == 12.0);
    CHECK(area(combine(c, bar, BooleanOp::subtract, tolerance)) == 16.0);
    CHECK(area(combine(c, bar, BooleanOp::exclude, tolerance)) == 32.0);

    // A square inside another, both clockwise: one filled square under nonzero, a frame under even-odd.
    Path nested = rectangle({0.0, 0.0}, {10.0, 10.0});
    nested.append(rectangle({3.0, 3.0}, {7.0, 7.0}));
    CHECK(area(combine(nested, Path{}, BooleanOp::unite, tolerance, FillRule::nonZero)) == 100.0);
    CHECK(area(combine(nested, Path{}, BooleanOp::unite, tolerance, FillRule::evenOdd)) == 84.0);

    // A pentagram drawn in one stroke winds twice around its center: nonzero fills the whole star, even-odd only its
    // five points.
    constexpr double outer = 15.0;
    const double inner = outer * std::cos(2.0 * std::numbers::pi / 5.0) / std::cos(std::numbers::pi / 5.0);
    std::vector<Vec2> corners;
    for (int i = 0; i < 5; ++i)
        corners.push_back(polar({20.0, 20.0}, outer, -std::numbers::pi / 2.0 + 4.0 * std::numbers::pi * i / 5.0));
    const double starArea = 5.0 * outer * inner * std::sin(std::numbers::pi / 5.0);
    const double pentagonArea = 2.5 * inner * inner * std::sin(2.0 * std::numbers::pi / 5.0);
    const Path pentagram = polygon(corners);
    const double budget = roundingBudget(pentagram, Path{});
    CHECK(std::abs(area(combine(pentagram, Path{}, BooleanOp::unite, tolerance, FillRule::nonZero)) - starArea) <=
          budget);
    const std::vector<Contour> points =
        contoursOf(combine(pentagram, Path{}, BooleanOp::unite, tolerance, FillRule::evenOdd));
    CHECK(points.size() == 5);
    CHECK(std::abs(area(points) - (starArea - pentagonArea)) <= budget);
}

TEST_CASE("Outer contours wind clockwise and holes counterclockwise inside them") {
    // A frame with an island in its hole.
    const Path frame =
        combine(rectangle({0.0, 0.0}, {10.0, 10.0}), rectangle({2.0, 2.0}, {8.0, 8.0}), BooleanOp::subtract, tolerance);
    const std::vector<Contour> contours =
        contoursOf(combine(frame, rectangle({4.0, 4.0}, {6.0, 6.0}), BooleanOp::unite, tolerance));
    REQUIRE(contours.size() == 3);
    std::vector<double> twiceAreas;
    for (const Contour &contour : contours)
        twiceAreas.push_back(twiceSignedArea(contour));
    std::sort(twiceAreas.begin(), twiceAreas.end());
    CHECK(twiceAreas == std::vector<double>{-72.0, 8.0, 200.0});
}

TEST_CASE("Results fill the same under either rule and match the inputs combined pixel by pixel" *
          doctest::test_suite("slow")) {
    // Pixel-aligned rectangles cover each pixel wholly or not at all, so coverage compares exactly.
    std::mt19937 rng(99);
    constexpr int trials = 40;
    constexpr int size = 24;
    constexpr int rectanglesEach = 4;
    const auto randomRectangles = [&] {
        const auto coordinate = [&] { return static_cast<double>(rng() % size); };
        Path path;
        for (int i = 0; i < rectanglesEach; ++i)
            path.append(rectangle({coordinate(), coordinate()}, {coordinate(), coordinate()}));
        return path;
    };
    for (int trial = 0; trial < trials; ++trial) {
        const Path a = randomRectangles();
        const Path b = randomRectangles();
        for (const FillRule rule : rules) {
            const std::vector<float> coverA = coverage(flatten(a, tolerance), size, size, rule);
            const std::vector<float> coverB = coverage(flatten(b, tolerance), size, size, rule);
            for (const BooleanOp op : operations) {
                const std::vector<Contour> result = contoursOf(combine(a, b, op, tolerance, rule));
                const std::vector<float> nonZero = coverage(result, size, size, FillRule::nonZero);
                CHECK(nonZero == coverage(result, size, size, FillRule::evenOdd));
                std::vector<float> expected(coverA.size());
                for (std::size_t i = 0; i < expected.size(); ++i)
                    expected[i] = combined(op, coverA[i] > 0.5f, coverB[i] > 0.5f) ? 1.0f : 0.0f;
                CHECK(nonZero == expected);
            }
        }
    }
}

TEST_CASE("Empty, flat and unfinished inputs give what they enclose, which may be nothing") {
    CHECK(combine(Path{}, Path{}, BooleanOp::unite, tolerance).empty());
    Path flat;
    flat.moveTo({0.0, 0.0}).lineTo({5.0, 5.0}).lineTo({10.0, 10.0}).close();
    CHECK(combine(flat, Path{}, BooleanOp::unite, tolerance).empty());
    CHECK(combine(rectangle({0.0, 0.0}, {1.0, 1.0}), rectangle({3.0, 3.0}, {4.0, 4.0}), BooleanOp::intersect, tolerance)
              .empty());
    // An open subpath fills as if closed, and a subpath with a point that is not finite is left out.
    Path open;
    open.moveTo({0.0, 0.0}).lineTo({4.0, 0.0}).lineTo({4.0, 4.0});
    open.moveTo({0.0, 0.0}).lineTo({std::numeric_limits<double>::quiet_NaN(), 1.0}).lineTo({1.0, 1.0}).close();
    CHECK(area(combine(open, Path{}, BooleanOp::unite, tolerance)) == 8.0);
}
