#include "support.hpp"

#include "linea/Shapes.hpp"
#include "linea/Stroke.hpp"

#include <doctest/doctest.h>

#include <numbers>

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

TEST_CASE("Every piece of a stroke winds clockwise, so the nonzero rule unites them") {
    for (const Contour &piece : strokeOutline(flatten(star({10.0, 10.0}, 8.0, 3.0, 5), 0.01), {.width = 1.5}, 0.01))
        CHECK(twiceSignedArea(piece) > 0.0);
    CHECK(strokeOutline(flatten(segment({0.0, 0.0}, {5.0, 5.0}), 0.1), {.width = 0.0}, 0.1).empty());
}
