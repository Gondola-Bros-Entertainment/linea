#include "linea/Drawing.hpp"
#include "linea/Patterns.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace linea;

namespace {

Color colorAt(const Image &image, int x, int y) {
    const std::uint8_t *p =
        image.rgba.data() +
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
    return {p[0], p[1], p[2], p[3]};
}

Layer filled(Path path, Paint paint) {
    Layer layer;
    layer.path = std::move(path);
    layer.fill = std::move(paint);
    return layer;
}

// @p paint filling a drawing @p units square, rendered @p zoom pixels a unit.
Image painted(Paint paint, int units, int zoom = 1) {
    const auto side = static_cast<double>(units);
    return render({side, side, {filled(rectangle({0.0, 0.0}, {side, side}), std::move(paint))}}, units * zoom,
                  units * zoom);
}

constexpr Color first{200, 30, 60, 255};
constexpr Color second{20, 120, 220, 255};
constexpr Color ink{30, 30, 30, 255};

// A quarter turn clockwise, written out because rotate() leaves rounding where its zeros should be, and the test below
// needs transforms whose products are exact.
constexpr Affine quarterTurn{0.0, 1.0, -1.0, 0.0, 0.0, 0.0};

// How many pixels of a checkerboard of squares @p size units on a side, rendered @p zoom pixels a unit, differ from
// the color of the square their center falls in.
int wrongSquares(const Image &image, double size, int zoom) {
    const auto square = [&](int pixel) { return static_cast<int>(std::floor((pixel + 0.5) / zoom / size)); };
    int wrong = 0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            if (colorAt(image, x, y) != ((square(x) + square(y)) % 2 == 0 ? first : second))
                ++wrong;
    return wrong;
}

} // namespace

TEST_CASE("A checkerboard gives exact squares at every whole scale, of the layer or of the pattern") {
    constexpr int units = 12;
    for (int zoom = 1; zoom <= 4; ++zoom) {
        CAPTURE(zoom);
        // The tile is drawn at the resolution the image needs, so its pixels land squarely on the image's.
        CHECK(wrongSquares(painted(checkerboard(2.0, first, second), units, zoom), 2.0, zoom) == 0);
        Pattern scaled = checkerboard(1.0, first, second);
        scaled.transform = scale(static_cast<double>(zoom));
        CHECK(wrongSquares(painted(scaled, units * zoom), static_cast<double>(zoom), 1) == 0);
    }
}

TEST_CASE("A rotated layer rotates its pattern with it") {
    // Stripes: the left half of each tile 4 units square in ink, the right half clear.
    const Pattern stripes{
        std::make_shared<const Drawing>(Drawing{4.0, 4.0, {filled(rectangle({0.0, 0.0}, {2.0, 4.0}), ink)}})};
    const Layer upright = filled(rectangle({0.0, 0.0}, {16.0, 16.0}), stripes);
    Layer turned = upright;
    // A quarter turn clockwise about the drawing's center, which takes (x, y) to (16 - y, x).
    turned.transform = translate({16.0, 0.0}) * quarterTurn;
    const Image before = render({16.0, 16.0, {upright}}, 32, 32);
    const Image after = render({16.0, 16.0, {turned}}, 32, 32);
    // Upright, the stripes run down the image.
    CHECK(colorAt(before, 1, 0) == ink);
    CHECK(colorAt(before, 1, 31) == ink);
    CHECK(colorAt(before, 5, 0).a == 0);
    // Turned, the image is the upright one turned: pixel (x, y) moves to (31 - y, x), and the stripes run across it.
    int moved = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            if (colorAt(after, 31 - y, x) != colorAt(before, x, y))
                ++moved;
    CHECK(moved == 0);
    CHECK(colorAt(after, 0, 1) == ink);
    CHECK(colorAt(after, 31, 1) == ink);
}

TEST_CASE("A pattern's transform moves its tiles, and each tile is cut to its rectangle") {
    // Moved 2 units right, a checkerboard of 2-unit squares starts with a square of its second color.
    Pattern shifted = checkerboard(2.0, first, second);
    shifted.transform = translate({2.0, 0.0});
    const Image moved = painted(shifted, 8);
    CHECK(colorAt(moved, 0, 0) == second);
    CHECK(colorAt(moved, 2, 0) == first);
    CHECK(colorAt(moved, 0, 2) == first);

    // A bar from x = -2 to 2 in a tile 4 units square: only the part from 0 to 2 lies in the tile and shows, so the
    // part left of the tile does not reach into the tile to the left, from 2 to 4.
    const Image bars = painted(
        Pattern{std::make_shared<const Drawing>(Drawing{4.0, 4.0, {filled(rectangle({-2.0, 1.0}, {2.0, 3.0}), ink)}})},
        8);
    CHECK(colorAt(bars, 1, 2) == ink);
    CHECK(colorAt(bars, 3, 2).a == 0);
    CHECK(colorAt(bars, 5, 2) == ink);
    CHECK(colorAt(bars, 7, 2).a == 0);
}

TEST_CASE("Dot grids, line grids and crosshatching lie on whole multiples of their spacing") {
    // Dots of radius 2 at every multiple of 10: the pixels around a lattice point lie wholly inside its dot, and one
    // 4 units from every lattice point lies outside them all.
    const Image dots = painted(dotGrid(10.0, 2.0, ink), 40);
    CHECK(colorAt(dots, 0, 0) == ink);
    CHECK(colorAt(dots, 9, 9) == ink);
    CHECK(colorAt(dots, 10, 30) == ink);
    CHECK(colorAt(dots, 39, 20) == ink);
    CHECK(colorAt(dots, 5, 20).a == 0);
    CHECK(colorAt(dots, 15, 15).a == 0);

    // Lines 2 wide on every multiple of 10: the band from 9 to 11 covers pixels 9 and 10 wholly and 11 not at all.
    const Image lines = painted(lineGrid(10.0, 2.0, ink), 40);
    CHECK(colorAt(lines, 9, 5) == ink);
    CHECK(colorAt(lines, 10, 5) == ink);
    CHECK(colorAt(lines, 11, 5).a == 0);
    CHECK(colorAt(lines, 0, 15) == ink);
    CHECK(colorAt(lines, 39, 15) == ink);
    CHECK(colorAt(lines, 5, 19) == ink);
    CHECK(colorAt(lines, 5, 21).a == 0);
    CHECK(colorAt(lines, 5, 5).a == 0);

    // Lines 3 wide along x - y = 10 k and x + y = 10 k: a pixel whose corners all lie within 1.5 of one is covered, and
    // one whose corners all lie farther from every one is clear. Pixels (2, 2) and (22, 32) lie on the first kind only,
    // (7, 2) and (12, 27) on the second only, and (0, 0), (10, 10) and (5, 5) where they cross.
    const Image hatch = painted(crosshatch(10.0, 3.0, ink), 40);
    for (const auto &[x, y] :
         std::vector<std::pair<int, int>>{{2, 2}, {22, 32}, {7, 2}, {12, 27}, {0, 0}, {10, 10}, {5, 5}})
        CHECK(colorAt(hatch, x, y) == ink);
    for (const auto &[x, y] : std::vector<std::pair<int, int>>{{5, 0}, {15, 0}, {20, 5}, {0, 25}})
        CHECK(colorAt(hatch, x, y).a == 0);
}

TEST_CASE(
    "A pattern without a tile, or whose tile has no area or nodes, or whose transform collapses it, paints nothing") {
    Pattern collapsed = checkerboard(2.0, first, second);
    collapsed.transform = scale(1.0, 0.0);
    for (const Pattern &pattern : {Pattern{}, checkerboard(0.0, first, second), collapsed,
                                   Pattern{std::make_shared<const Drawing>(Drawing{4.0, 4.0, {}})}}) {
        const Image image = painted(pattern, 8);
        int shown = 0;
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            if (image.rgba[i] != 0)
                ++shown;
        CHECK(shown == 0);
    }
}
