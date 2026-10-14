#include "linea/Measure.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace linea;

namespace {

// Intervals for the reference integration. A multiple of 3, so that t = 1/3 falls where two of Simpson's panels meet,
// and enough that the rule's error, checked against 30-digit integration, is below 1e-12 for every curve here.
constexpr int simpsonIntervals = 3 * 4096;

// The length of the curve whose derivative is @p velocity, by composite Simpson integration of its speed over [0, 1].
template <typename Velocity> double simpsonLength(Velocity velocity) {
    const double h = 1.0 / simpsonIntervals;
    double sum = length(velocity(0.0)) + length(velocity(1.0));
    for (int i = 1; i < simpsonIntervals; ++i)
        sum += (i % 2 == 1 ? 4.0 : 2.0) * length(velocity(static_cast<double>(i) * h));
    return sum * h / 3.0;
}

Path cubicPath(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
    Path path;
    path.moveTo(p0).cubicTo(p1, p2, p3);
    return path;
}

} // namespace

TEST_CASE("A polyline measures exactly the sum of its sides, the side that closes it included") {
    Path path;
    // Sides of 3, 4 and 5, then 10 and 12.
    path.moveTo({1.0, 1.0}).lineTo({4.0, 1.0}).lineTo({4.0, 5.0}).close();
    path.moveTo({0.0, 10.0}).lineTo({6.0, 18.0}).lineTo({6.0, 30.0});
    CHECK(arcLength(path) == 34.0);
    // Sides a double cannot hold exactly sum, in order, to exactly what the path measures. Quadrature of these sides'
    // constant speeds would come out an ulp off.
    const std::vector<Vec2> corners{{11.0, 16.0}, {8.0, 14.0}, {3.0, 18.0}, {11.0, 9.0}, {1.0, 13.0}, {2.0, 6.0}};
    double sides = 0.0;
    for (std::size_t i = 0; i < corners.size(); ++i)
        sides += length(corners[(i + 1) % corners.size()] - corners[i]);
    CHECK(arcLength(polygon(corners)) == sides);
    CHECK(arcLength(Path{}) == 0.0);
}

TEST_CASE("Curves measure within 1e-9 of a dense numeric reference, a cusp included") {
    // An S-shaped cubic, and one whose speed falls to zero at t = 1/3, a cusp, where the speed has a corner.
    const std::array<std::array<Vec2, 4>, 2> cubics{{
        {{{0.0, 0.0}, {30.0, -80.0}, {90.0, 120.0}, {140.0, 10.0}}},
        {{{0.0, 0.0}, {40.0, 40.0}, {-10.0, 20.0}, {30.0, -60.0}}},
    }};
    for (const std::array<Vec2, 4> &p : cubics) {
        const double reference = simpsonLength([&](double t) {
            const double s = 1.0 - t;
            return (p[1] - p[0]) * (3.0 * s * s) + (p[2] - p[1]) * (6.0 * s * t) + (p[3] - p[2]) * (3.0 * t * t);
        });
        CHECK(std::abs(arcLength(cubicPath(p[0], p[1], p[2], p[3])) - reference) < 1e-9);
    }
    const Vec2 q0{0.0, 0.0};
    const Vec2 q1{60.0, -90.0};
    const Vec2 q2{100.0, 30.0};
    Path quad;
    quad.moveTo(q0).quadTo(q1, q2);
    const double reference =
        simpsonLength([&](double t) { return (q1 - q0) * (2.0 * (1.0 - t)) + (q2 - q1) * (2.0 * t); });
    CHECK(std::abs(arcLength(quad) - reference) < 1e-9);
}

TEST_CASE("The point at 0 and at the length is exactly the path's first and last point, and nothing lies beyond") {
    Path path;
    path.moveTo({1.0, 2.0}).lineTo({7.0, 10.0}).moveTo({20.0, 20.0}).lineTo({20.0, 26.0});
    const double total = arcLength(path);
    REQUIRE(total == 16.0);
    const PathPoint first = pointAt(path, 0.0).value();
    CHECK(first.point == Vec2{1.0, 2.0});
    CHECK(first.tangent == Vec2{0.6, 0.8});
    const PathPoint last = pointAt(path, total).value();
    CHECK(last.point == Vec2{20.0, 26.0});
    CHECK(last.tangent == Vec2{0.0, 1.0});
    // A corner belongs to the segment arriving there, and the distance runs on through each subpath in turn.
    CHECK(pointAt(path, 10.0).value().tangent == Vec2{0.6, 0.8});
    CHECK(pointAt(path, 13.0).value().point == Vec2{20.0, 23.0});
    CHECK_FALSE(pointAt(path, std::nextafter(total, 2.0 * total)));
    CHECK_FALSE(pointAt(path, -std::numeric_limits<double>::denorm_min()));
    CHECK_FALSE(pointAt(path, std::numeric_limits<double>::quiet_NaN()));

    const Vec2 p0{0.0, 0.0};
    const Vec2 p3{140.0, 10.0};
    const Path curve = cubicPath(p0, {30.0, -80.0}, {90.0, 120.0}, p3);
    CHECK(pointAt(curve, 0.0).value().point == p0);
    CHECK(pointAt(curve, arcLength(curve)).value().point == p3);

    // A path without length has nowhere to be.
    Path dot;
    dot.moveTo({3.0, 3.0}).lineTo({3.0, 3.0});
    CHECK_FALSE(pointAt(dot, 0.0));
    CHECK_FALSE(pointAt(Path{}, 0.0));
}

TEST_CASE("A point along a curve lies at the distance asked, however unevenly the curve's parameter runs") {
    // A straight cubic along x from 0 to 30 whose speed falls fourfold along it: the point d along it is at x = d.
    const Path path = cubicPath({0.0, 0.0}, {20.0, 0.0}, {25.0, 0.0}, {30.0, 0.0});
    CHECK(std::abs(arcLength(path) - 30.0) < 1e-9);
    for (const double d : {0.5, 3.0, 7.5, 12.0, 21.25, 29.0}) {
        const PathPoint p = pointAt(path, d).value();
        CHECK(std::abs(p.point.x - d) < 1e-9);
        CHECK(p.point.y == 0.0);
        CHECK(p.tangent == Vec2{1.0, 0.0});
    }
    // Through a cusp, 26.0995 along this curve, where the speed falls to 0 with a corner: the part before each point
    // measures the distance to it.
    const Path cusp = cubicPath({0.0, 0.0}, {40.0, 40.0}, {-10.0, 20.0}, {30.0, -60.0});
    for (const double d : {20.0, 26.0995, 26.1, 26.2, 60.0})
        CHECK(std::abs(arcLength(splitAt(cusp, d).before) - d) < 1e-9);
}

TEST_CASE("Where a curve stops for an instant at an end, the tangent is the way it leaves or arrives") {
    // Each end's control point sits on it, so the curve runs from (0, 0) to (30, 40) and is still at both ends.
    const Path path = cubicPath({0.0, 0.0}, {0.0, 0.0}, {30.0, 40.0}, {30.0, 40.0});
    CHECK(pointAt(path, 0.0).value().tangent == Vec2{0.6, 0.8});
    CHECK(pointAt(path, arcLength(path)).value().tangent == Vec2{0.6, 0.8});
}

TEST_CASE("A path split at a distance comes apart in two parts that meet at the point there and add up to the whole") {
    Path path;
    path.moveTo({0.0, 0.0}).cubicTo({30.0, -80.0}, {90.0, 120.0}, {140.0, 10.0});
    path.quadTo({160.0, 60.0}, {120.0, 90.0}).lineTo({100.0, 70.0}).close();
    path.moveTo({200.0, 0.0}).lineTo({230.0, 40.0});
    const double total = arcLength(path);
    const double firstCurve = arcLength(cubicPath({0.0, 0.0}, {30.0, -80.0}, {90.0, 120.0}, {140.0, 10.0}));
    // Within the cubic, at its end, within the quadratic, within the closing line, and within the second subpath.
    for (const double d : {50.0, firstCurve, firstCurve + 20.0, total - 60.0, total - 20.0}) {
        const PathParts parts = splitAt(path, d);
        const Vec2 cut = pointAt(path, d).value().point;
        CHECK(parts.before.current() == cut);
        CHECK(parts.after.points().front() == cut);
        // The cut is found to within 1e-12 of the cut curve's control polygon, which is under 500 long here.
        CHECK(std::abs(arcLength(parts.before) - d) < 5e-10);
        CHECK(arcLength(parts.before) + arcLength(parts.after) == doctest::Approx(total).epsilon(1e-12));
    }
    // A closed subpath cut in two is open in both parts, the part after running on around to its start.
    const PathParts opened = splitAt(path, 50.0);
    CHECK(opened.before.verbs() == std::vector{Verb::move, Verb::cubic});
    CHECK(opened.after.verbs() ==
          std::vector{Verb::move, Verb::cubic, Verb::quad, Verb::line, Verb::line, Verb::move, Verb::line});
    CHECK(opened.after.points()[7] == Vec2{0.0, 0.0});
}

TEST_CASE("A split where a subpath ends falls between subpaths, and one at either end leaves all on one side") {
    Path first;
    first.moveTo({0.0, 0.0}).lineTo({30.0, 0.0}).lineTo({30.0, 40.0}).close();
    Path second;
    second.moveTo({100.0, 0.0}).quadTo({120.0, 30.0}, {140.0, 0.0});
    Path path = first;
    path.append(second);
    const PathParts between = splitAt(path, 120.0);
    CHECK(between.before == first);
    CHECK(between.after == second);
    CHECK(splitAt(path, 0.0).after == path);
    CHECK(splitAt(path, 0.0).before.empty());
    CHECK(splitAt(path, arcLength(path)).before == path);
    CHECK(splitAt(path, arcLength(path)).after.empty());
}
