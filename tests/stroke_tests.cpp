#include "support.hpp"

#include "linea/Shapes.hpp"
#include "linea/Stroke.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using namespace linea;
using namespace linea::test;

namespace {

double strokedArea(const Path &path, const StrokeStyle &style, int size = 20) {
    return total(coverage(strokeOutline(flatten(path, 0.01), style, 0.001), size, size, FillRule::nonZero));
}

Path segment(Vec2 a, Vec2 b) {
    Path path;
    path.moveTo(a).lineTo(b);
    return path;
}

} // namespace

TEST_CASE("Caps add exactly their own area to an open stroke") {
    const Path line = segment({4.0, 10.0}, {14.0, 10.0});
    CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::butt}) == doctest::Approx(20.0).epsilon(1e-9));
    CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::square}) == doctest::Approx(24.0).epsilon(1e-9));
    CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::round}) ==
          doctest::Approx(20.0 + std::numbers::pi).epsilon(1e-4));
}

TEST_CASE("Joins fill a closed stroke's corners as asked") {
    const Path square = rectangle({4.0, 4.0}, {14.0, 14.0});
    // Mitered, the stroke is the band between the squares 3 to 15 and 5 to 13.
    CHECK(strokedArea(square, {.width = 2.0, .join = Join::miter}) == doctest::Approx(144.0 - 64.0).epsilon(1e-9));
    // A bevel cuts a half-unit triangle off each of the four corners.
    CHECK(strokedArea(square, {.width = 2.0, .join = Join::bevel}) == doctest::Approx(80.0 - 4.0 * 0.5).epsilon(1e-9));
    // A round join is a quarter circle at each corner in place of the miter's unit square.
    CHECK(strokedArea(square, {.width = 2.0, .join = Join::round}) ==
          doctest::Approx(80.0 - 4.0 * (1.0 - std::numbers::pi / 4.0)).epsilon(1e-4));
}

TEST_CASE("A miter longer than the limit becomes a bevel") {
    Path sharp;
    sharp.moveTo({2.0, 18.0}).lineTo({10.0, 2.0}).lineTo({18.0, 18.0});
    const double longMiter =
        strokedArea(sharp, {.width = 2.0, .join = Join::miter, .cap = Cap::butt, .miterLimit = 10.0});
    const double limited = strokedArea(sharp, {.width = 2.0, .join = Join::miter, .cap = Cap::butt, .miterLimit = 1.5});
    const double bevelled = strokedArea(sharp, {.width = 2.0, .join = Join::bevel, .cap = Cap::butt});
    CHECK(longMiter > bevelled);
    CHECK(limited == doctest::Approx(bevelled).epsilon(1e-9));
}

TEST_CASE("A stroke's outline does not overlap itself, so it fills the same under either rule") {
    // The star's stroke has a hole, which winds counterclockwise inside the outer contour.
    const std::vector<Contour> outline =
        strokeOutline(flatten(star({10.0, 10.0}, 8.0, 3.0, 5), 0.01), {.width = 1.5}, 0.01);
    CHECK(std::ranges::any_of(outline, [](const Contour &c) { return twiceSignedArea(c) > 0.0; }));
    CHECK(std::ranges::any_of(outline, [](const Contour &c) { return twiceSignedArea(c) < 0.0; }));
    const std::vector<float> nonZero = coverage(outline, 20, 20, FillRule::nonZero);
    const std::vector<float> evenOdd = coverage(outline, 20, 20, FillRule::evenOdd);
    for (std::size_t i = 0; i < nonZero.size(); ++i)
        CHECK(nonZero[i] == doctest::Approx(evenOdd[i]).epsilon(1e-6));
    CHECK(strokeOutline(flatten(segment({0.0, 0.0}, {5.0, 5.0}), 0.1), {.width = 0.0}, 0.1).empty());
}

TEST_CASE("Where a stroke's pieces meet within a pixel, it covers the exact area of their union there") {
    // A round-capped stroke along y = 5.3, 3 wide, from x = 2 to x = 7.4. Its top and bottom edges cross pixel rows, so
    // each cap meets the band inside pixels it covers only partly. Over x the stroke covers the band between the caps'
    // centers and each cap's chord beyond, so integrating over x gives each pixel's exact covered area.
    constexpr double from = 2.0;
    constexpr double to = 7.4;
    constexpr double middle = 5.3;
    constexpr double half = 1.5;
    const auto exact = [&](int px, int py) {
        constexpr int slices = 20000;
        double area = 0.0;
        for (int i = 0; i < slices; ++i) {
            const double x = px + (i + 0.5) / slices;
            const double beyond = x < from ? from - x : x > to ? x - to : 0.0;
            if (beyond >= half)
                continue;
            const double reach = std::sqrt(half * half - beyond * beyond);
            area +=
                std::max(0.0, std::min(middle + reach, py + 1.0) - std::max(middle - reach, static_cast<double>(py)));
        }
        return area / slices;
    };
    const std::vector<float> cover =
        coverage(strokeOutline(flatten(segment({from, middle}, {to, middle}), 1e-4), {.width = 2.0 * half}, 1e-4), 10,
                 8, FillRule::nonZero);
    double worst = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 10; ++x)
            worst = std::max(worst, std::abs(at(cover, 10, x, y) - exact(x, y)));
    // The caps are polygons within 1e-4 of their circles, so a pixel's coverage strays far less than a step of 255.
    CHECK(worst < 1e-3);
}

TEST_CASE("A stroke wider than its closed path covers the path widened by half the width, once") {
    // Every point of the square lies within half the width of its edges, so the stroke covers the square widened by
    // 1.5 with round corners, s^2 + 4 s h + pi h^2, though its pieces overlap everywhere inside.
    constexpr double side = 2.0;
    constexpr double half = 1.5;
    const double area = total(coverage(
        strokeOutline(flatten(rectangle({9.0, 9.0}, {9.0 + side, 9.0 + side}), 1e-4), {.width = 2.0 * half}, 1e-4), 20,
        20, FillRule::nonZero));
    CHECK(area == doctest::Approx(side * side + 4.0 * side * half + std::numbers::pi * half * half).epsilon(1e-5));
}

TEST_CASE("A dashed stroke with butt caps covers the dashed fraction of the solid stroke's area") {
    // Four repeats of a dash of 3 and a gap of 1 along a line 16 long: three quarters of it.
    const Path line = segment({2.0, 10.0}, {18.0, 10.0});
    const StrokeStyle solid{.width = 2.0, .cap = Cap::butt};
    StrokeStyle dashed = solid;
    dashed.dashArray = {3.0, 1.0};
    CHECK(strokedArea(line, solid) == doctest::Approx(32.0).epsilon(1e-9));
    CHECK(strokedArea(line, dashed) == doctest::Approx(0.75 * 32.0).epsilon(1e-9));
    // An odd array repeats to an even one, so 1.5, 0.5 and 1 dash half of every 6. A slanted line 18 long holds three
    // repeats, so whatever the offset, negative too, half of it is dashed. Off the axes, an outline's corners snap to
    // the boolean grid, within tolerance / 128 on each axis, which moves an area by at most its perimeter times that:
    // here under 1e-4 of it.
    const Path slant = segment({1.0, 2.0}, {15.4, 12.8});
    dashed.dashArray = {1.5, 0.5, 1.0};
    dashed.dashOffset = -2.25;
    CHECK(strokedArea(slant, dashed) == doctest::Approx(0.5 * strokedArea(slant, solid)).epsilon(1e-4));
}

TEST_CASE("A dash array that is empty, sums to zero or holds a negative length strokes solid") {
    const Path line = segment({2.0, 10.0}, {18.0, 10.0});
    const std::vector<std::vector<double>> arrays{{}, {0.0, 0.0}, {-1.0, 3.0}};
    for (const std::vector<double> &array : arrays)
        CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::butt, .dashArray = array}) ==
              doctest::Approx(32.0).epsilon(1e-9));
}

TEST_CASE("A closed path's dashes start exactly at its first point, and each subpath starts the pattern afresh") {
    // Dashes of 3 and gaps of 2 around a square 40 long, from its first point at (4, 4): the first dash runs along the
    // top from x = 4 to 7, and the last gap runs up the left side into the first point, leaving its corner empty.
    const StrokeStyle style{.width = 2.0, .join = Join::miter, .cap = Cap::butt, .dashArray = {3.0, 2.0}};
    const std::vector<float> square = coverage(
        strokeOutline(flatten(rectangle({4.0, 4.0}, {14.0, 14.0}), 0.01), style, 0.001), 20, 20, FillRule::nonZero);
    CHECK(at(square, 20, 4, 3) == 1.0);
    CHECK(at(square, 20, 6, 4) == 1.0);
    CHECK(at(square, 20, 7, 3) == 0.0);
    CHECK(at(square, 20, 3, 3) == 0.0);
    CHECK(at(square, 20, 3, 4) == 0.0);
    CHECK(at(square, 20, 3, 5) == 0.0);
    // Two lines 4 long: were the pattern to run on from the first, the second would start 1 into a gap.
    Path lines;
    lines.moveTo({2.0, 2.0}).lineTo({6.0, 2.0}).moveTo({2.0, 6.0}).lineTo({6.0, 6.0});
    const std::vector<float> two =
        coverage(strokeOutline(flatten(lines, 0.01), style, 0.001), 10, 10, FillRule::nonZero);
    CHECK(at(two, 10, 2, 1) == 1.0);
    CHECK(at(two, 10, 2, 5) == 1.0);
    CHECK(at(two, 10, 5, 5) == 0.0);
}

TEST_CASE("On a closed path, a dash that runs on through the first point joins there") {
    // Offset 2 into dashes of 3 and gaps of 2, the square's pattern is on from 38 to its end at 40 and on again from 0
    // to 1: one dash, around the first corner, which the miter fills.
    StrokeStyle style{.width = 2.0, .join = Join::miter, .cap = Cap::butt, .dashArray = {3.0, 2.0}, .dashOffset = 2.0};
    const Path square = rectangle({4.0, 4.0}, {14.0, 14.0});
    const std::vector<float> cover =
        coverage(strokeOutline(flatten(square, 0.01), style, 0.001), 20, 20, FillRule::nonZero);
    CHECK(at(cover, 20, 3, 3) == doctest::Approx(1.0));
    CHECK(at(cover, 20, 3, 4) == 1.0);
    CHECK(at(cover, 20, 4, 3) == 1.0);
    CHECK(at(cover, 20, 5, 3) == 0.0);
    // A dash longer than the way around is the solid stroke, with no caps where it would meet itself.
    style.dashArray = {50.0, 1.0};
    style.dashOffset = 0.0;
    CHECK(strokedArea(square, style) == doctest::Approx(144.0 - 64.0).epsilon(1e-9));
}

TEST_CASE("Dashes of no length draw their caps as dots, a square one turned along the path") {
    // Dots 5 apart along a line 20 long, at 0, 5, 10 and 15, but none at 20, where the last gap ends.
    const Path line = segment({2.0, 10.0}, {22.0, 10.0});
    CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::round, .dashArray = {0.0, 5.0}}, 30) ==
          doctest::Approx(4.0 * std::numbers::pi).epsilon(1e-4));
    CHECK(strokedArea(line, {.width = 2.0, .cap = Cap::square, .dashArray = {0.0, 5.0}}, 30) ==
          doctest::Approx(4.0 * 4.0).epsilon(1e-9));
    // One square dot on a diagonal stands on its corner: it reaches only the far corner of the pixel that an upright
    // one would fill, leaving a corner of 3 - 2 sqrt(2) bare. Its corners snap to the boolean grid as the slanted
    // dashes' do, which moves its area by under 1e-4 of it.
    const std::vector<float> diamond =
        coverage(strokeOutline(flatten(segment({10.0, 10.0}, {20.0, 20.0}), 0.01),
                               {.width = 2.0, .cap = Cap::square, .dashArray = {0.0, 100.0}}, 0.001),
                 20, 20, FillRule::nonZero);
    CHECK(total(diamond) == doctest::Approx(4.0).epsilon(1e-4));
    CHECK(at(diamond, 20, 9, 9) == doctest::Approx(2.0 * std::numbers::sqrt2 - 2.0).epsilon(1e-6));
}
