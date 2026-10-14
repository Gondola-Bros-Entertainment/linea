#include "linea/Flatten.hpp"
#include "linea/Noise.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using namespace linea;

namespace {

// Pinned values hold to within floating-point rounding, which is far below this.
constexpr double pinned = 1e-9;
// The steepest slopes the noises reach, rounded up: Perlin noise's, 2.9262 at a cell's center, and simplex noise's,
// 3 sqrt 6.
constexpr double perlinSlope = 2.93;
constexpr double simplexSlope = 3.0 * std::numbers::sqrt3 * std::numbers::sqrt2;
// The most simplex noise bends along any line, per unit squared, rounded up from 55.78.
constexpr double simplexBend = 56.0;
// Rounding in a difference of two samples a step apart, far below any slope these tests look for.
constexpr double rounding = 1e-12;

double distanceToClosedPolyline(Vec2 p, const std::vector<Vec2> &line) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < line.size(); ++i) {
        const Vec2 a = line[i];
        const Vec2 ab = line[(i + 1) % line.size()] - a;
        const double t = std::clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
        best = std::min(best, length(p - (a + ab * t)));
    }
    return best;
}

// How sharply a closed polyline bends at point @p i: its turn there over the mean length of its two sides.
double bendAt(const std::vector<Vec2> &line, std::size_t i) {
    const std::size_t n = line.size();
    const Vec2 in = line[i] - line[(i + n - 1) % n];
    const Vec2 out = line[(i + 1) % n] - line[i];
    return std::abs(std::atan2(cross(in, out), dot(in, out))) / ((length(in) + length(out)) / 2.0);
}

double sharpestBend(const std::vector<Vec2> &line) {
    double sharpest = 0.0;
    for (std::size_t i = 0; i < line.size(); ++i)
        sharpest = std::max(sharpest, bendAt(line, i));
    return sharpest;
}

} // namespace

TEST_CASE("Perlin and simplex noise are pinned to known values for each seed") {
    CHECK(perlin({0.3, 0.7}, 0) == doctest::Approx(0.383341257511).epsilon(pinned));
    CHECK(perlin({0.3, 0.7}, 7) == doctest::Approx(-0.416949372518).epsilon(pinned));
    CHECK(perlin({12.25, -3.5}, 0) == doctest::Approx(-0.278091143916).epsilon(pinned));
    CHECK(perlin({-7.8, 4.1}, 7) == doctest::Approx(0.164645638536).epsilon(pinned));
    CHECK(simplex({0.3, 0.7}, 0) == doctest::Approx(-0.312053002404).epsilon(pinned));
    CHECK(simplex({0.3, 0.7}, 7) == doctest::Approx(-0.456413686774).epsilon(pinned));
    CHECK(simplex({12.25, -3.5}, 0) == doctest::Approx(-0.736123749585).epsilon(pinned));
    CHECK(simplex({-7.8, 4.1}, 7) == doctest::Approx(0.332276330158).epsilon(pinned));
}

TEST_CASE("Perlin noise is 0 at every lattice point, and simplex noise at every corner of its triangles") {
    for (const std::uint32_t seed : {0u, 7u, 0xffffffffu}) {
        for (int y = -3; y <= 3; ++y) {
            for (int x = -3; x <= 3; ++x)
                CHECK(perlin({static_cast<double>(x), static_cast<double>(y)}, seed) == 0.0);
            CHECK(simplex({static_cast<double>(y), static_cast<double>(-y)}, seed) == 0.0);
        }
        // Lattice coordinates far beyond 32 bits wrap rather than overflow.
        CHECK(perlin({1e12, -3e15}, seed) == 0.0);
    }
    // A point that is not finite has no noise.
    CHECK(perlin({std::numeric_limits<double>::quiet_NaN(), 1.0}, 0) == 0.0);
    CHECK(simplex({1.0, std::numeric_limits<double>::infinity()}, 0) == 0.0);
}

TEST_CASE("Noise stays within -1 to 1 and comes near both ends") {
    double perlinLow = 0.0;
    double perlinHigh = 0.0;
    double simplexLow = 0.0;
    double simplexHigh = 0.0;
    for (const std::uint32_t seed : {0u, 1u}) {
        for (int i = 0; i < 800; ++i) {
            for (int j = 0; j < 800; ++j) {
                const Vec2 p{-20.0 + i * 0.05 + 0.0013, -20.0 + j * 0.05 + 0.0071};
                const double a = perlin(p, seed);
                const double b = simplex(p, seed);
                perlinLow = std::min(perlinLow, a);
                perlinHigh = std::max(perlinHigh, a);
                simplexLow = std::min(simplexLow, b);
                simplexHigh = std::max(simplexHigh, b);
            }
        }
    }
    CHECK(perlinLow >= -1.0);
    CHECK(perlinHigh <= 1.0);
    CHECK(simplexLow >= -1.0);
    CHECK(simplexHigh <= 1.0);
    // Both are scaled to their true peaks, so they use nearly the whole range.
    CHECK(perlinLow < -0.9);
    CHECK(perlinHigh > 0.9);
    CHECK(simplexLow < -0.9);
    CHECK(simplexHigh > 0.9);
}

TEST_CASE("Noise is continuous: neighbouring samples differ by no more than its steepest slope allows") {
    // A slanted walk across hundreds of lattice cells: any seam between cells would jump far beyond the slope.
    constexpr double step = 1e-3;
    const Vec2 along = normalized({1.0, 0.37});
    double perlinSteepest = 0.0;
    double simplexSteepest = 0.0;
    for (const std::uint32_t seed : {0u, 1u, 2u}) {
        for (int i = 0; i < 200000; ++i) {
            const Vec2 a = Vec2{-50.0, 3.3} + along * (i * step);
            const Vec2 b = a + along * step;
            perlinSteepest = std::max(perlinSteepest, std::abs(perlin(b, seed) - perlin(a, seed)));
            simplexSteepest = std::max(simplexSteepest, std::abs(simplex(b, seed) - simplex(a, seed)));
        }
    }
    CHECK(perlinSteepest <= perlinSlope * step + rounding);
    CHECK(simplexSteepest <= simplexSlope * step + rounding);
}

TEST_CASE("Fractal noise sums octaves of its noise, weighted by their amplitudes") {
    const Vec2 p{3.7, -1.2};
    CHECK(fbm(perlin, p, 5, {.octaves = 1}) == perlin(p, 5));
    CHECK(fbm(simplex, p, 5, {.octaves = 1}) == simplex(p, 5));
    CHECK(fbm(simplex, p, 5, {.octaves = 0}) == 0.0);
    // Octave i samples at lacunarity^i times the frequency with seed + i, weighted by gain^i, over the sum of the
    // weights' magnitudes.
    const double expected = (perlin(p, 5) - 0.5 * perlin(p * 3.0, 6) + 0.25 * perlin(p * 9.0, 7)) / 1.75;
    CHECK(fbm(perlin, p, 5, {.octaves = 3, .lacunarity = 3.0, .gain = -0.5}) == doctest::Approx(expected));
    CHECK(fbm(simplex, {0.3, 0.7}, 0, {.octaves = 5}) == doctest::Approx(-0.432247075848).epsilon(pinned));
    double low = 0.0;
    double high = 0.0;
    for (int i = 0; i < 200; ++i) {
        for (int j = 0; j < 200; ++j) {
            const double v = fbm(simplex, {i * 0.1, j * 0.1}, 3, {.octaves = 6});
            low = std::min(low, v);
            high = std::max(high, v);
        }
    }
    CHECK(low >= -1.0);
    CHECK(high <= 1.0);
}

TEST_CASE("Voronoi distances are continuous and ordered, and a cell's id changes only on its boundary") {
    const Voronoi known = voronoi({0.3, 0.7}, 7);
    CHECK(known.nearest == doctest::Approx(0.593828539154).epsilon(pinned));
    CHECK(known.secondNearest == doctest::Approx(0.672079773598).epsilon(pinned));
    CHECK(known.id == 512537842u);

    // Distances to points change no faster than the point moves, so a feature point that the search missed on one
    // side of a cell's edge and found on the other would show as a jump. Both distances are equal on a boundary, so
    // where the id changes between two samples a step apart, they differ by at most two steps at each.
    constexpr double step = 1e-3;
    const Vec2 along = normalized({0.83, 1.0});
    double nearestSteepest = 0.0;
    double secondSteepest = 0.0;
    double farthestNearest = 0.0;
    double farthestSecond = 0.0;
    bool ordered = true;
    bool idsChangeOnBoundaries = true;
    for (const std::uint32_t seed : {0u, 7u}) {
        Voronoi before = voronoi({-40.0, -31.0}, seed);
        for (int i = 1; i < 100000; ++i) {
            const Voronoi now = voronoi(Vec2{-40.0, -31.0} + along * (i * step), seed);
            nearestSteepest = std::max(nearestSteepest, std::abs(now.nearest - before.nearest));
            secondSteepest = std::max(secondSteepest, std::abs(now.secondNearest - before.secondNearest));
            farthestNearest = std::max(farthestNearest, now.nearest);
            farthestSecond = std::max(farthestSecond, now.secondNearest);
            ordered = ordered && now.nearest <= now.secondNearest;
            if (now.id != before.id)
                idsChangeOnBoundaries = idsChangeOnBoundaries && now.secondNearest - now.nearest <= 2.0 * step &&
                                        before.secondNearest - before.nearest <= 2.0 * step;
            before = now;
        }
    }
    CHECK(nearestSteepest <= step + rounding);
    CHECK(secondSteepest <= step + rounding);
    CHECK(ordered);
    CHECK(idsChangeOnBoundaries);
    // A cell's own feature point is within sqrt 2 of anywhere in it, and two of the four cells around the nearest
    // lattice corner hold feature points within sqrt 3.25.
    CHECK(farthestNearest <= std::numbers::sqrt2);
    CHECK(farthestSecond <= std::sqrt(3.25));

    const Voronoi nowhere = voronoi({std::numeric_limits<double>::quiet_NaN(), 0.0}, 0);
    CHECK(std::isinf(nowhere.nearest));
    CHECK(std::isinf(nowhere.secondNearest));
    CHECK(nowhere.id == 0u);
}

TEST_CASE("Voronoi distances are exact on both sides of a lattice cell's edge, where the search moves over a cell") {
    // The search looks at cells around the point's own, so it moves over a cell wherever the point crosses a cell's
    // edge. One that missed a feature point on one side of the edge would find it on the other, and jump.
    constexpr double nudge = 1e-9;
    double worstJump = 0.0;
    for (const std::uint32_t seed : {0u, 7u}) {
        for (int edge = -16; edge < 16; ++edge) {
            for (int j = 0; j < 256; ++j) {
                const double across = -16.0 + j / 8.0 + 0.0625;
                const double at = static_cast<double>(edge);
                for (const auto &[before, after] : {std::pair{Vec2{at - nudge, across}, Vec2{at + nudge, across}},
                                                    std::pair{Vec2{across, at - nudge}, Vec2{across, at + nudge}}}) {
                    const Voronoi a = voronoi(before, seed);
                    const Voronoi b = voronoi(after, seed);
                    worstJump = std::max(
                        {worstJump, std::abs(a.nearest - b.nearest), std::abs(a.secondNearest - b.secondNearest)});
                }
            }
        }
    }
    CHECK(worstJump <= 2.0 * nudge + rounding);
}

TEST_CASE("A wobbled line moves straight across itself, by noise that strays at most the tolerance between points") {
    Path line;
    line.moveTo({0.0, 0.0}).lineTo({100.0, 0.0});
    const Wobble wobble{.amplitude = 4.0, .frequency = 0.2, .seed = 3};
    // Points along a line all move along its normal, so every point of a wobbled line is an exact sample of the
    // noise's displacement, and a fine wobble samples it densely.
    const std::vector<Contour> fine = flatten(wobbled(line, wobble, 0.001), 0.001);
    REQUIRE(fine.size() == 1);
    CHECK_FALSE(fine.front().closed);
    for (const double tolerance : {0.5, 0.1, 0.01}) {
        const std::vector<Vec2> coarse = flatten(wobbled(line, wobble, tolerance), tolerance).front().points;
        REQUIRE(coarse.size() > 2);
        CHECK(coarse.front().x == 0.0);
        CHECK(coarse.back().x == 100.0);
        bool across = true;
        double farthest = 0.0;
        double worstBend = 0.0;
        for (std::size_t i = 1; i < coarse.size(); ++i) {
            across = across && coarse[i].x > coarse[i - 1].x;
            farthest = std::max(farthest, std::abs(coarse[i].y));
            // Twice the second divided difference of exact samples is the displacement's second derivative somewhere
            // between them, which the noise's bend bounds.
            if (i + 1 < coarse.size()) {
                const Vec2 a = coarse[i - 1];
                const Vec2 b = coarse[i];
                const Vec2 c = coarse[i + 1];
                const double turn = (c.y - b.y) / (c.x - b.x) - (b.y - a.y) / (b.x - a.x);
                worstBend = std::max(worstBend, std::abs(2.0 * turn / (c.x - a.x)));
            }
        }
        CHECK(across);
        CHECK(farthest <= wobble.amplitude);
        CHECK(worstBend <= wobble.amplitude * wobble.frequency * wobble.frequency * simplexBend);
        // Between its points, the coarse line strays from the noise by at most the tolerance.
        double worstGap = 0.0;
        std::size_t segment = 0;
        for (const Vec2 sample : fine.front().points) {
            while (segment + 2 < coarse.size() && coarse[segment + 1].x < sample.x)
                ++segment;
            const Vec2 a = coarse[segment];
            const Vec2 b = coarse[segment + 1];
            const double y = a.y + (b.y - a.y) * (sample.x - a.x) / (b.x - a.x);
            worstGap = std::max(worstGap, std::abs(y - sample.y));
        }
        CHECK(worstGap <= tolerance);
    }
}

TEST_CASE("A wobbled circle stays one closed contour near its circle, and converges as the tolerance shrinks") {
    constexpr double radius = 40.0;
    const Wobble wobble{.amplitude = 2.0, .frequency = 0.1, .seed = 7};
    constexpr double fineTolerance = 1e-4;
    const std::vector<Vec2> fine =
        flatten(wobbled(circle({0.0, 0.0}, radius), wobble, fineTolerance), fineTolerance).front().points;
    for (const double tolerance : {1.0, 0.1, 0.01}) {
        const Path path = wobbled(circle({0.0, 0.0}, radius), wobble, tolerance);
        CHECK(path.verbs().back() == Verb::close);
        const std::vector<Contour> outline = flatten(path, tolerance);
        REQUIRE(outline.size() == 1);
        CHECK(outline.front().closed);
        // Flattening strays at most the tolerance from the circle, and the noise between points at most the
        // tolerance from where it moves them, so each point lies within the amplitude of the circle, give or take
        // both, and within both of the fine outline, give or take the fine one's.
        double nearest = radius;
        double farthest = radius;
        double worstGap = 0.0;
        for (const Vec2 p : outline.front().points) {
            nearest = std::min(nearest, length(p));
            farthest = std::max(farthest, length(p));
            worstGap = std::max(worstGap, distanceToClosedPolyline(p, fine));
        }
        CHECK(nearest >= radius - wobble.amplitude - 2.0 * tolerance);
        CHECK(farthest <= radius + wobble.amplitude + 2.0 * tolerance);
        CHECK(farthest - nearest > wobble.amplitude);
        CHECK(worstGap <= 2.0 * (tolerance + fineTolerance));
    }
}

TEST_CASE("A wobbled circle bends smoothly, at its seam as everywhere else") {
    // A smooth outline's sharpest bend, its turn per unit length, settles to the curve's own curvature as the points
    // close in, where a kink's would grow as the points' spacing shrinks. The seam is the contour's first point.
    const Wobble wobble{.amplitude = 2.0, .frequency = 0.1, .seed = 7};
    const std::vector<Vec2> coarse = flatten(wobbled(circle({0.0, 0.0}, 40.0), wobble, 0.01), 0.01).front().points;
    const std::vector<Vec2> fine = flatten(wobbled(circle({0.0, 0.0}, 40.0), wobble, 0.001), 0.001).front().points;
    REQUIRE(fine.size() > 2 * coarse.size());
    CHECK(sharpestBend(fine) == doctest::Approx(sharpestBend(coarse)).epsilon(0.1));
    CHECK(bendAt(fine, 0) == doctest::Approx(bendAt(coarse, 0)).epsilon(0.1));
}

TEST_CASE("A wobbled corner lands where its sides meet once each has moved, even at the origin") {
    // Simplex noise is 0 at the origin, so without the seed's shift this corner would stay put for every seed.
    const Vec2 corners[] = {{0.0, 0.0}, {60.0, 10.0}, {10.0, 50.0}};
    const Wobble wobble{.amplitude = 3.0, .frequency = 0.1, .seed = 11};
    const Vec2 moved = wobbled(polygon(corners), wobble, 0.1).points().front();
    const Vec2 intoSide = perpendicular(normalized(corners[0] - corners[2]));
    const Vec2 outOfSide = perpendicular(normalized(corners[1] - corners[0]));
    const double offset = dot(moved - corners[0], intoSide);
    CHECK(dot(moved - corners[0], outOfSide) == doctest::Approx(offset));
    CHECK(std::abs(offset) > 0.0);
    CHECK(std::abs(offset) <= wobble.amplitude);
}

TEST_CASE("Wobbling without amplitude only flattens the path, and a seed always wobbles the same way") {
    Path path = ring({50.0, 50.0}, 30.0, 12.0);
    path.moveTo({0.0, 0.0}).quadTo({20.0, -30.0}, {40.0, 5.0});
    const std::vector<Contour> flat = flatten(path, 0.1);
    const std::vector<Contour> still =
        flatten(wobbled(path, {.amplitude = 0.0, .frequency = 0.1, .seed = 1}, 0.1), 0.1);
    REQUIRE(still.size() == flat.size());
    for (std::size_t i = 0; i < flat.size(); ++i) {
        CHECK(still[i].points == flat[i].points);
        CHECK(still[i].closed == flat[i].closed);
    }
    const Path pinnedPath = wobbled(circle({0.0, 0.0}, 10.0), {.amplitude = 2.0, .frequency = 0.25, .seed = 42}, 0.1);
    CHECK(pinnedPath.points().front().x == doctest::Approx(10.965896372685).epsilon(pinned));
    CHECK(pinnedPath.points().front().y == doctest::Approx(0.0));
}
