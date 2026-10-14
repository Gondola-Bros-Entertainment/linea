#include "support.hpp"

#include "linea/Flatten.hpp"
#include "linea/Raster.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <limits>
#include <numbers>

using namespace linea;
using namespace linea::test;

namespace {

std::vector<float> fill(const Path &path, int width, int height, FillRule rule = FillRule::nonZero) {
    return coverage(flatten(path, 0.01), width, height, rule);
}

} // namespace

TEST_CASE("A pixel-aligned rectangle covers exactly its pixels") {
    const std::vector<float> cover = fill(rectangle({2.0, 3.0}, {6.0, 5.0}), 10, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            CHECK(at(cover, 10, x, y) == ((x >= 2 && x < 6 && y >= 3 && y < 5) ? 1.0 : 0.0));
}

TEST_CASE("Coverage is the exact area of each pixel inside the shape") {
    const std::vector<float> half = fill(rectangle({2.5, 0.0}, {6.0, 1.0}), 10, 1);
    CHECK(at(half, 10, 2, 0) == doctest::Approx(0.5));
    CHECK(at(half, 10, 3, 0) == doctest::Approx(1.0));
    const Vec2 corners[] = {{0.0, 0.0}, {10.0, 0.0}, {0.0, 10.0}};
    CHECK(total(fill(polygon(corners), 10, 10)) == doctest::Approx(50.0).epsilon(1e-9));
    constexpr double r = 20.0;
    CHECK(total(fill(circle({32.0, 32.0}, r), 64, 64)) == doctest::Approx(std::numbers::pi * r * r).epsilon(1e-3));
}

TEST_CASE("Shapes beyond the image keep exact coverage inside it and leak nowhere") {
    const std::vector<float> topLeft = fill(rectangle({-5.0, -5.0}, {5.0, 5.0}), 10, 10);
    CHECK(total(topLeft) == doctest::Approx(25.0));
    CHECK(at(topLeft, 10, 0, 0) == 1.0);
    CHECK(at(topLeft, 10, 5, 0) == 0.0);
    const std::vector<float> bottomRight = fill(rectangle({5.0, 5.0}, {15.0, 15.0}), 10, 10);
    CHECK(total(bottomRight) == doctest::Approx(25.0));
    // Nothing that crosses the right border spills into the start of the next row.
    CHECK(at(bottomRight, 10, 0, 6) == 0.0);
    CHECK(total(fill(rectangle({-50.0, -50.0}, {-10.0, -10.0}), 10, 10)) == 0.0);
}

TEST_CASE("A slanted edge that crosses the image's left or right side keeps its slope inside the image") {
    // The triangle (5, 0), (15, 10), (5, 10) lies left of x = 5 + y, which leaves the image at (10, 5): of its area of
    // 50, the corner from there to (15, 10) lies outside, 12.5, and pixel (7, 3) lies wholly inside.
    const Vec2 rightward[] = {{5.0, 0.0}, {15.0, 10.0}, {5.0, 10.0}};
    const std::vector<float> right = fill(polygon(rightward), 10, 10);
    CHECK(total(right) == doctest::Approx(37.5).epsilon(1e-9));
    CHECK(at(right, 10, 7, 3) == doctest::Approx(1.0));
    // Its mirror image across x = 5 crosses the left side.
    const Vec2 leftward[] = {{5.0, 0.0}, {5.0, 10.0}, {-5.0, 10.0}};
    const std::vector<float> left = fill(polygon(leftward), 10, 10);
    CHECK(total(left) == doctest::Approx(37.5).epsilon(1e-9));
    CHECK(at(left, 10, 2, 3) == doctest::Approx(1.0));
    // An edge from (20, 0) to (-10, 10), y = (20 - x) / 3, crosses both sides. Pixel (5, 4) lies under it between
    // x = 5, where it is at y = 5, and x = 6, where it is at 14 / 3: an area of 5 / 6.
    const Vec2 across[] = {{-10.0, 0.0}, {20.0, 0.0}, {-10.0, 10.0}};
    CHECK(at(fill(polygon(across), 10, 10), 10, 5, 4) == doctest::Approx(5.0 / 6.0));
}

TEST_CASE("The fill rules differ where contours wind around a point more than once") {
    // A pentagram drawn in one stroke winds twice around its center.
    std::vector<Vec2> points;
    for (int i = 0; i < 5; ++i)
        points.push_back(polar({20.0, 20.0}, 15.0, -std::numbers::pi / 2.0 + 4.0 * std::numbers::pi * i / 5.0));
    const Path pentagram = polygon(points);
    // Slanted edges sum to coverage with rounding that differs between compilers, far below what an 8-bit image shows.
    CHECK(at(fill(pentagram, 40, 40, FillRule::nonZero), 40, 20, 20) == doctest::Approx(1.0));
    CHECK(at(fill(pentagram, 40, 40, FillRule::evenOdd), 40, 20, 20) == doctest::Approx(0.0));
    // A ring's hole is empty under either rule.
    for (const FillRule rule : {FillRule::nonZero, FillRule::evenOdd}) {
        const std::vector<float> band = fill(ring({20.0, 20.0}, 15.0, 8.0), 40, 40, rule);
        CHECK(at(band, 40, 20, 20) == doctest::Approx(0.0));
        CHECK(at(band, 40, 20, 8) == doctest::Approx(1.0));
    }
}

TEST_CASE("Nothing to draw, or a contour with a point that is not finite, covers nothing") {
    CHECK(total(coverage({}, 4, 4, FillRule::nonZero)) == 0.0);
    const Contour broken{{{0.0, 0.0}, {4.0, std::numeric_limits<double>::quiet_NaN()}, {0.0, 4.0}}, true};
    CHECK(total(coverage({broken}, 4, 4, FillRule::nonZero)) == 0.0);
    CHECK(coverage({}, 0, -3, FillRule::nonZero).size() == 1);
}
