#include "linea/Flatten.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace linea;

namespace {

double distanceToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double t = std::clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - (a + ab * t));
}

double distanceToPolyline(Vec2 p, const std::vector<Vec2> &line) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
        best = std::min(best, distanceToSegment(p, line[i], line[i + 1]));
    return best;
}

} // namespace

TEST_CASE("A flattened curve never strays from the true curve by more than the tolerance") {
    const Vec2 p0{0.0, 0.0}, p1{30.0, -80.0}, p2{90.0, 120.0}, p3{140.0, 10.0};
    for (const double tolerance : {1.0, 0.25, 0.05}) {
        Path path;
        path.moveTo(p0).cubicTo(p1, p2, p3);
        const std::vector<Vec2> line = flatten(path, tolerance).front().points;
        CHECK(line.front() == p0);
        CHECK(line.back() == p3);
        double worst = 0.0;
        constexpr int samples = 4000;
        for (int i = 0; i <= samples; ++i)
            worst =
                std::max(worst, distanceToPolyline(evalCubic(p0, p1, p2, p3, static_cast<double>(i) / samples), line));
        CHECK(worst <= tolerance);
    }
}

TEST_CASE("Lines flatten to their own points, and a closed contour drops a repeated last point") {
    Path path;
    path.moveTo({1.0, 1.0}).lineTo({4.0, 1.0}).lineTo({4.0, 4.0}).lineTo({1.0, 1.0}).close();
    const std::vector<Contour> contours = flatten(path, 0.1);
    REQUIRE(contours.size() == 1);
    CHECK(contours.front().closed);
    CHECK(contours.front().points == std::vector<Vec2>{{1.0, 1.0}, {4.0, 1.0}, {4.0, 4.0}});
}

TEST_CASE("Shapes wind clockwise, and a ring's hole winds the other way") {
    CHECK(twiceSignedArea(flatten(rectangle({0.0, 0.0}, {4.0, 3.0}), 0.1).front()) == doctest::Approx(24.0));
    const std::vector<Contour> band = flatten(ring({0.0, 0.0}, 10.0, 6.0), 0.01);
    REQUIRE(band.size() == 2);
    CHECK(twiceSignedArea(band[0]) > 0.0);
    CHECK(twiceSignedArea(band[1]) < 0.0);
    const std::vector<Contour> round = flatten(circle({3.0, 4.0}, 25.0), 0.01);
    for (const Vec2 p : round.front().points)
        CHECK(length(p - Vec2{3.0, 4.0}) == doctest::Approx(25.0).epsilon(3e-4));
}
