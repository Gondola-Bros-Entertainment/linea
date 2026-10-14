#include "linea/Flatten.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

double distanceToContour(Vec2 p, const Contour &contour) {
    const double open = distanceToPolyline(p, contour.points);
    return contour.closed ? std::min(open, distanceToSegment(p, contour.points.back(), contour.points.front())) : open;
}

// Whether every point of @p part appears in @p whole, in the same order.
bool isSubsequence(const std::vector<Vec2> &part, const std::vector<Vec2> &whole) {
    auto next = whole.begin();
    for (const Vec2 p : part) {
        next = std::find(next, whole.end(), p);
        if (next == whole.end())
            return false;
        ++next;
    }
    return true;
}

// The farthest that any point of @p original lies from @p simple.
double worstDistance(const Contour &original, const Contour &simple) {
    double worst = 0.0;
    for (const Vec2 p : original.points)
        worst = std::max(worst, distanceToContour(p, simple));
    return worst;
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

TEST_CASE("The two halves of a split curve trace the original exactly and meet at the point at t") {
    const Vec2 p0{0.0, 0.0}, p1{30.0, -80.0}, p2{90.0, 120.0}, p3{140.0, 10.0};
    constexpr int samples = 1000;
    for (const double t : {0.25, 0.5, 0.8}) {
        const std::array<Vec2, 7> cubic = splitCubic(p0, p1, p2, p3, t);
        const std::array<Vec2, 5> quad = splitQuad(p0, p1, p2, t);
        CHECK(cubic.front() == p0);
        CHECK(cubic.back() == p3);
        CHECK(quad.front() == p0);
        CHECK(quad.back() == p2);
        // The first half at u is the original at t u, and the second half at u the original at t + (1 - t) u.
        double worst = 0.0;
        for (int i = 0; i <= samples; ++i) {
            const double u = static_cast<double>(i) / samples;
            const double before = t * u;
            const double after = t + (1.0 - t) * u;
            for (const double gap :
                 {length(evalCubic(cubic[0], cubic[1], cubic[2], cubic[3], u) - evalCubic(p0, p1, p2, p3, before)),
                  length(evalCubic(cubic[3], cubic[4], cubic[5], cubic[6], u) - evalCubic(p0, p1, p2, p3, after)),
                  length(evalQuad(quad[0], quad[1], quad[2], u) - evalQuad(p0, p1, p2, before)),
                  length(evalQuad(quad[2], quad[3], quad[4], u) - evalQuad(p0, p1, p2, after))})
                worst = std::max(worst, gap);
        }
        CHECK(worst == doctest::Approx(0.0).epsilon(1e-12));
    }
}

TEST_CASE("Splitting a curve at 0 or 1 gives the whole curve on one side and a single point on the other") {
    // Coordinates where a + (b - a) rounds away from b, so stepping from one end would miss the other.
    const Vec2 p0{1.1, -2.9}, p1{0.3, 0.7}, p2{0.7, 0.1}, p3{-2.9, 3.3};
    CHECK(splitCubic(p0, p1, p2, p3, 0.0) == std::array<Vec2, 7>{p0, p0, p0, p0, p1, p2, p3});
    CHECK(splitCubic(p0, p1, p2, p3, 1.0) == std::array<Vec2, 7>{p0, p1, p2, p3, p3, p3, p3});
    CHECK(splitQuad(p0, p1, p2, 0.0) == std::array<Vec2, 5>{p0, p0, p0, p1, p2});
    CHECK(splitQuad(p0, p1, p2, 1.0) == std::array<Vec2, 5>{p0, p1, p2, p2, p2});
}

TEST_CASE("A simplified polyline keeps its ends, and every point of the original lies within the tolerance of it") {
    // A wave with a little regular jitter, the kind of input simplification is for.
    Contour wave;
    for (int i = 0; i <= 400; ++i) {
        const double x = 0.5 * i;
        wave.points.push_back({x, 20.0 * std::sin(x / 15.0) + 0.05 * ((i * 7) % 5 - 2)});
    }
    for (const double tolerance : {0.25, 1.0, 4.0}) {
        const Contour simple = simplified(wave, tolerance);
        CHECK_FALSE(simple.closed);
        CHECK(simple.points.front() == wave.points.front());
        CHECK(simple.points.back() == wave.points.back());
        CHECK(isSubsequence(simple.points, wave.points));
        CHECK(simple.points.size() < wave.points.size());
        CHECK(worstDistance(wave, simple) <= tolerance);
    }
    // A point past the end of the segment between its neighbors lies on the line through them but far from the
    // segment, so it stays.
    const Contour overshoot{{{0.0, 0.0}, {10.0, 0.0}, {4.0, 0.0}}, false};
    CHECK(simplified(overshoot, 1.0).points == overshoot.points);
    // Points along a straight line add nothing to it.
    const Contour straight{{{0.0, 0.0}, {1.0, 2.0}, {2.0, 4.0}, {3.0, 6.0}}, false};
    CHECK(simplified(straight, 1e-9).points == std::vector<Vec2>{{0.0, 0.0}, {3.0, 6.0}});
}

TEST_CASE("A closed contour simplifies around its whole loop, not from its first point") {
    // A square traced from the middle of its top edge, with a point in the middle of every edge. Its first and last
    // points would stay as the ends of an open polyline; around the loop, only its corners need to.
    const Contour square{
        {{5.0, 0.0}, {10.0, 0.0}, {10.0, 5.0}, {10.0, 10.0}, {5.0, 10.0}, {0.0, 10.0}, {0.0, 5.0}, {0.0, 0.0}}, true};
    const Contour corners = simplified(square, 0.01);
    CHECK(corners.closed);
    CHECK(corners.points == std::vector<Vec2>{{10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.0, 0.0}});

    const Contour round = flatten(circle({0.0, 0.0}, 50.0), 0.001).front();
    for (const double tolerance : {0.01, 0.5, 5.0}) {
        const Contour simple = simplified(round, tolerance);
        CHECK(simple.closed);
        CHECK(isSubsequence(simple.points, round.points));
        CHECK(simple.points.size() < round.points.size());
        CHECK(worstDistance(round, simple) <= tolerance);
    }
}
