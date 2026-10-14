#include "linea/Flatten.hpp"
#include "linea/Path.hpp"

#include <doctest/doctest.h>

#include <cmath>

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
