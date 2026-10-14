#include "linea/Flatten.hpp"
#include "linea/Path.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace linea;

TEST_CASE("A path records its verbs and points, and draws from the origin before any move") {
    Path path;
    path.lineTo({4.0, 0.0}).quadTo({6.0, 0.0}, {6.0, 2.0}).cubicTo({6.0, 4.0}, {2.0, 4.0}, {0.0, 2.0}).close();
    CHECK(path.verbs() == std::vector{Verb::move, Verb::line, Verb::quad, Verb::cubic, Verb::close});
    CHECK(path.points().front() == Vec2{0.0, 0.0});
    CHECK(path.points().size() == 7);
    CHECK(path.current() == Vec2{0.0, 0.0});
}

TEST_CASE("Drawing after a close starts a new subpath at the closed one's start") {
    Path path;
    path.moveTo({1.0, 1.0}).lineTo({5.0, 1.0}).lineTo({5.0, 5.0}).close().lineTo({1.0, 9.0});
    CHECK(path.verbs() == std::vector{Verb::move, Verb::line, Verb::line, Verb::close, Verb::move, Verb::line});
    CHECK(path.points()[3] == Vec2{1.0, 1.0});
}

TEST_CASE("An arc lies on its circle and sweeps the way it is asked to") {
    constexpr double r = 10.0;
    Path path;
    path.moveTo({-r, 0.0}).arcTo({r, r}, 0.0, false, true, {r, 0.0});
    double topmost = 0.0;
    const std::vector<Contour> contours = flatten(path, 0.001);
    for (const Vec2 p : contours.front().points) {
        CHECK(length(p) == doctest::Approx(r).epsilon(3e-4));
        topmost = std::min(topmost, p.y);
    }
    // Clockwise on a y-down canvas, from 9 o'clock to 3 o'clock, passes through 12 o'clock, where y is negative.
    CHECK(topmost == doctest::Approx(-r).epsilon(1e-6));
    CHECK(path.current() == Vec2{r, 0.0});
}

TEST_CASE("Arc radii too small to reach scale up, and a zero radius draws a line") {
    Path grown;
    grown.moveTo({0.0, 0.0}).arcTo({1.0, 1.0}, 0.0, false, true, {10.0, 0.0});
    const std::vector<Contour> contours = flatten(grown, 0.001);
    for (const Vec2 p : contours.front().points)
        CHECK(length(p - Vec2{5.0, 0.0}) == doctest::Approx(5.0).epsilon(3e-4));
    Path flat;
    flat.moveTo({0.0, 0.0}).arcTo({0.0, 4.0}, 0.0, true, true, {10.0, 0.0});
    CHECK(flat.verbs() == std::vector{Verb::move, Verb::line});
    // Ends so far apart that the arithmetic overflows a double leave no angle to sweep, and draw a line.
    Path overflowing;
    overflowing.moveTo({-1e308, 0.0}).arcTo({1.0, 1.0}, 0.0, false, true, {1e308, 0.0});
    CHECK(overflowing.verbs() == std::vector{Verb::move, Verb::line});
    CHECK(overflowing.current() == Vec2{1e308, 0.0});
}

TEST_CASE("A transformed path moves every point, and its bounds hold them all") {
    Path path;
    path.moveTo({0.0, 0.0}).cubicTo({4.0, -3.0}, {8.0, 3.0}, {12.0, 0.0});
    const Path moved = transformed(path, translate({1.0, 2.0}));
    CHECK(moved.points()[2] == Vec2{9.0, 5.0});
    const Bounds bounds = controlBounds(path);
    CHECK(bounds.min == Vec2{0.0, -3.0});
    CHECK(bounds.max == Vec2{12.0, 3.0});
}

TEST_CASE("Reversing a path twice gives back the same path") {
    Path path;
    path.moveTo({1.0, 2.0})
        .lineTo({6.0, 2.0})
        .quadTo({9.0, 0.0}, {9.0, 5.0})
        .cubicTo({9.0, 9.0}, {3.0, 11.0}, {1.0, 7.0})
        .moveTo({20.0, 20.0})
        .moveTo({30.0, 30.0})
        .close();
    path.append(roundedRectangle({50.0, 50.0}, {10.0, 6.0}, 3.0, 0.4)).append(star({80.0, 20.0}, 9.0, 4.0, 5));
    // Drawing after the star's close starts one more subpath, at the star's first point.
    path.lineTo({0.0, 0.0});
    const Path back = reversed(path);
    CHECK(back != path);
    CHECK(reversed(back) == path);
    CHECK(reversed(Path{}).empty());
}

TEST_CASE("A reversed path traces each subpath backward, the last one first, and closed subpaths stay closed") {
    Path path;
    path.moveTo({0.0, 0.0})
        .lineTo({4.0, 0.0})
        .quadTo({8.0, 0.0}, {8.0, 4.0})
        .cubicTo({8.0, 10.0}, {-2.0, 6.0}, {2.0, 12.0});
    path.append(rectangle({20.0, 0.0}, {26.0, 4.0}));
    const std::vector<Contour> forward = flatten(path, 0.01);
    const std::vector<Contour> backward = flatten(reversed(path), 0.01);
    REQUIRE(forward.size() == 2);
    REQUIRE(backward.size() == 2);
    // The rectangle comes first, still closed, with its corners in the opposite order, so it winds the other way.
    CHECK(backward[0].closed);
    CHECK(backward[0].points == std::vector<Vec2>(forward[1].points.rbegin(), forward[1].points.rend()));
    CHECK(twiceSignedArea(backward[0]) == -twiceSignedArea(forward[1]));
    // The open subpath's lines and curves pass through the same points in the opposite order: a reversed curve at t is
    // the original at 1 - t.
    CHECK_FALSE(backward[1].closed);
    const std::vector<Vec2> &there = forward[0].points;
    const std::vector<Vec2> &back = backward[1].points;
    REQUIRE(back.size() == there.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < back.size(); ++i)
        worst = std::max(worst, length(back[i] - there[there.size() - 1 - i]));
    CHECK(worst == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("Tight bounds reach as far as the curves do, and no farther than their control points") {
    // A quadratic that bulges to y = 5 at t = 0.5, toward a control point at y = 10.
    Path arch;
    arch.moveTo({0.0, 0.0}).quadTo({5.0, 10.0}, {10.0, 0.0});
    CHECK(tightBounds(arch).min == Vec2{0.0, 0.0});
    CHECK(tightBounds(arch).max == Vec2{10.0, 5.0});
    CHECK(controlBounds(arch).max == Vec2{10.0, 10.0});

    // Curves that turn back in x or y between their ends, under control points that stick out past them on every side:
    // a cubic and a quadratic, and a rotated ellipse, whose arcs are cubics.
    Path curves;
    curves.moveTo({10.0, 40.0})
        .cubicTo({-30.0, -20.0}, {90.0, 110.0}, {60.0, 20.0})
        .quadTo({100.0, -15.0}, {75.0, 60.0})
        .lineTo({30.0, 70.0});
    for (const Path &path : {curves, ellipse({50.0, 50.0}, {30.0, 12.0}, 0.6)}) {
        const Bounds tight = tightBounds(path);
        const Bounds control = controlBounds(path);
        CHECK(tight.min.x > control.min.x);
        CHECK(tight.min.y > control.min.y);
        CHECK(tight.max.x < control.max.x);
        CHECK(tight.max.y < control.max.y);

        // A flattened curve strays at most the tolerance from its polyline, whose corners are evenly spaced samples of
        // the curve, so the samples come within the tolerance of every extreme. None of these curves is fine enough to
        // reach flatten's cap on segments, which would loosen that.
        constexpr double tolerance = 1e-4;
        Bounds sampled{path.points().front(), path.points().front()};
        for (const Contour &contour : flatten(path, tolerance)) {
            for (const Vec2 p : contour.points) {
                sampled.min = {std::min(sampled.min.x, p.x), std::min(sampled.min.y, p.y)};
                sampled.max = {std::max(sampled.max.x, p.x), std::max(sampled.max.y, p.y)};
            }
        }
        // No sample lies outside the tight bounds, beyond rounding.
        CHECK(sampled.min.x >= doctest::Approx(tight.min.x).epsilon(1e-12));
        CHECK(sampled.min.y >= doctest::Approx(tight.min.y).epsilon(1e-12));
        CHECK(sampled.max.x <= doctest::Approx(tight.max.x).epsilon(1e-12));
        CHECK(sampled.max.y <= doctest::Approx(tight.max.y).epsilon(1e-12));
        // And the tight bounds reach no farther than the samples do, give or take the tolerance.
        CHECK(sampled.min.x - tight.min.x <= tolerance);
        CHECK(sampled.min.y - tight.min.y <= tolerance);
        CHECK(tight.max.x - sampled.max.x <= tolerance);
        CHECK(tight.max.y - sampled.max.y <= tolerance);
    }
}

TEST_CASE("Tight bounds never reach past the control bounds, even where rounding would carry a curve past them") {
    // Curves whose control points lie within a few units in the last place of one another in y, so that each curve's
    // extreme sits at or right beside a control point, where rounding in evaluating it can land on either side.
    const auto step = [](double from, int places, double toward) {
        for (int i = 0; i < places; ++i)
            from = std::nextafter(from, toward);
        return from;
    };
    constexpr int peaks = 200;
    constexpr int mostPlaces = 4;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    int outside = 0;
    for (int i = 0; i < peaks; ++i) {
        const double peak = 0.7 * i - 70.0;
        // Below the peak it is a curve's greatest y, and above it its least.
        for (const double away : {-infinity, infinity}) {
            for (int a = 0; a < mostPlaces; ++a) {
                for (int b = 0; b < mostPlaces; ++b) {
                    for (int c = 0; c < mostPlaces; ++c) {
                        Path quad;
                        quad.moveTo({0.0, step(peak, a, away)}).quadTo({1.0, peak}, {2.0, step(peak, b, away)});
                        Path cubic;
                        cubic.moveTo({0.0, step(peak, a, away)})
                            .cubicTo({1.0, peak}, {2.0, step(peak, b, away)}, {3.0, step(peak, c, away)});
                        for (const Path &path : {quad, cubic}) {
                            const Bounds tight = tightBounds(path);
                            const Bounds control = controlBounds(path);
                            if (tight.min.x < control.min.x || tight.min.y < control.min.y ||
                                tight.max.x > control.max.x || tight.max.y > control.max.y)
                                ++outside;
                        }
                    }
                }
            }
        }
    }
    CHECK(outside == 0);
}
