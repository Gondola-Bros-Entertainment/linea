#include "support.hpp"

#include "linea/Drawing.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <utility>
#include <vector>

using namespace linea;
using namespace linea::test;

namespace {

constexpr Color red{255, 0, 0, 255};
constexpr Color white{255, 255, 255, 255};
constexpr Color black{0, 0, 0, 255};
constexpr Color blue{0, 0, 255, 255};

} // namespace

TEST_CASE("A solid fill paints its pixels and leaves the rest transparent") {
    const Image image = render({10.0, 10.0, {filled(rectangle({2.0, 2.0}, {6.0, 6.0}), red)}}, 10, 10);
    CHECK(image.width == 10);
    CHECK(image.rgba.size() == 400);
    const std::uint8_t *inside = pixel(image, 3, 3);
    CHECK((inside[0] == 255 && inside[1] == 0 && inside[2] == 0 && inside[3] == 255));
    CHECK(pixel(image, 8, 8)[3] == 0);
}

TEST_CASE("The drawing scales to the image, and layers move by their transforms") {
    Layer moved = filled(rectangle({0.0, 0.0}, {2.0, 2.0}), red);
    moved.transform = translate({4.0, 4.0});
    const Image image = render({10.0, 10.0, {moved}}, 20, 20);
    CHECK(pixel(image, 9, 9)[3] == 255);
    CHECK(pixel(image, 7, 7)[3] == 0);
    CHECK(pixel(image, 12, 12)[3] == 0);
}

TEST_CASE("Layers composite in linear light, and opacity scales them") {
    Layer veil = filled(rectangle({0.0, 0.0}, {4.0, 4.0}), white);
    veil.opacity = 0.5;
    const Image image = render({4.0, 4.0, {filled(rectangle({0.0, 0.0}, {4.0, 4.0}), black), veil}}, 4, 4);
    // Half of white's light over black is linear 0.5, which sRGB encodes as 188, not 128.
    CHECK(pixel(image, 1, 1)[0] == 188);
    CHECK(pixel(image, 1, 1)[3] == 255);
    const Image alone = render({4.0, 4.0, {veil}}, 4, 4);
    CHECK(alone.rgba[3] == 128);
}

TEST_CASE("Gradients blend in Oklab between their stops") {
    const LinearGradient ramp{{0.0, 0.0}, {100.0, 0.0}, evenStops({black, white})};
    const Image image = render({100.0, 1.0, {filled(rectangle({0.0, 0.0}, {100.0, 1.0}), ramp)}}, 100, 1);
    CHECK(pixel(image, 0, 0)[0] <= 2);
    CHECK(pixel(image, 99, 0)[0] >= 253);
    // Oklab's halfway lightness from black to white is linear 0.125, sRGB 99: perceptually halfway, where an sRGB
    // blend would give 128.
    CHECK(static_cast<int>(pixel(image, 49, 0)[0]) == doctest::Approx(99).epsilon(0.03));
    for (int x = 1; x < 100; ++x)
        CHECK(pixel(image, x, 0)[0] >= pixel(image, x - 1, 0)[0]);
}

TEST_CASE("A stroke paints along the path, and an empty drawing renders transparent") {
    Layer outline;
    outline.path = rectangle({2.0, 2.0}, {8.0, 8.0});
    outline.stroke = Paint{red};
    outline.strokeStyle = {.width = 2.0, .join = Join::miter};
    const Image image = render({10.0, 10.0, {outline}}, 10, 10);
    CHECK(pixel(image, 1, 5)[3] == 255);
    CHECK(pixel(image, 5, 5)[3] == 0);
    const Image empty = render({0.0, 10.0, {outline}}, 3, 3);
    for (const std::uint8_t byte : empty.rgba)
        CHECK(byte == 0);
}

TEST_CASE("A group placed twice matches its layers transformed by hand") {
    // A part of a gradient-filled, stroked body and a mark with a transform of its own. Every transform here multiplies
    // exactly, so composing them in either grouping gives exactly the same matrices.
    Layer body =
        filled(rectangle({0.0, 0.0}, {6.0, 3.0}), LinearGradient{{0.0, 0.0}, {6.0, 0.0}, evenStops({red, blue})});
    body.stroke = Paint{black};
    body.strokeStyle = {.width = 1.0, .join = Join::miter};
    Layer mark = filled(rectangle({0.0, 0.0}, {1.0, 2.0}), white);
    mark.transform = translate({4.0, 0.5});
    const Group part{.children = {body, mark}, .transform = translate({2.0, 0.0}) * scale(1.0, 2.0)};
    const Affine first = translate({4.0, 4.0});
    const Affine second = translate({34.0, 10.0}) * quarterTurn;
    const Image grouped = render(
        {40.0, 40.0, {Group{.children = {part}, .transform = first}, Group{.children = {part}, .transform = second}}},
        80, 80);

    std::vector<Node> byHand;
    for (const Affine &placement : {first, second}) {
        for (Layer layer : {body, mark}) {
            layer.transform = placement * part.transform * layer.transform;
            byHand.emplace_back(std::move(layer));
        }
    }
    CHECK(grouped.rgba == render({40.0, 40.0, byHand}, 80, 80).rgba);
    // Each placement lands where its transforms send it, so the match is not one of two empty images.
    CHECK(pixel(grouped, 18, 14)[3] == 255);
    CHECK(pixel(grouped, 62, 30)[3] == 255);
    CHECK(pixel(grouped, 40, 60)[3] == 0);
}

TEST_CASE("Overlapping layers in a half-transparent group do not show through each other") {
    const Layer left = filled(rectangle({0.0, 0.0}, {6.0, 4.0}), red);
    const Layer right = filled(rectangle({2.0, 0.0}, {8.0, 4.0}), blue);
    const Group veil{.children = {left, right}, .opacity = 0.5};
    // Alone, the overlap is half of blue, with none of the red beneath it.
    const Image alone = render({8.0, 4.0, {veil}}, 8, 4);
    CHECK(colorAt(alone, 4, 2) == Color{0, 0, 255, 128});
    CHECK(colorAt(alone, 1, 2) == Color{255, 0, 0, 128});
    // Over white, the overlap lets half of white's light through in red and green: linear 0.5, which sRGB encodes as
    // 188.
    const Image overWhite = render({8.0, 4.0, {filled(rectangle({0.0, 0.0}, {8.0, 4.0}), white), veil}}, 8, 4);
    CHECK(colorAt(overWhite, 4, 2) == Color{188, 188, 255, 255});
    CHECK(colorAt(overWhite, 1, 2) == Color{255, 188, 188, 255});
}

TEST_CASE("A half-transparent layer fades its fill and stroke together, so the fill does not show through the stroke") {
    Layer badge = filled(rectangle({2.0, 2.0}, {8.0, 8.0}), red);
    badge.stroke = Paint{blue};
    badge.strokeStyle = {.width = 2.0, .join = Join::miter};
    badge.opacity = 0.5;
    const Image image = render({10.0, 10.0, {badge}}, 10, 10);
    // The stroke's band from x 1 to 3 covers pixel 2 wholly, as the fill does: half of blue, with none of the red.
    CHECK(colorAt(image, 2, 5) == Color{0, 0, 255, 128});
    CHECK(colorAt(image, 1, 5) == Color{0, 0, 255, 128});
    CHECK(colorAt(image, 5, 5) == Color{255, 0, 0, 128});
}

TEST_CASE("Opacity multiplies through nested groups and their layers, and a transparent group draws nothing") {
    Layer square = filled(rectangle({0.0, 0.0}, {1.0, 1.0}), blue);
    square.opacity = 0.5;
    const Group inner{.children = {square}, .transform = translate({1.0, 0.0}), .opacity = 0.5};
    const Image image =
        render({2.0, 2.0, {Group{.children = {inner}, .transform = translate({0.0, 1.0}), .opacity = 0.5}}}, 2, 2);
    // An eighth of blue, moved by both groups: alpha 255 / 8 = 31.875 rounds to 32, and the color stays blue.
    CHECK(colorAt(image, 1, 1) == Color{0, 0, 255, 32});
    CHECK(pixel(image, 0, 0)[3] == 0);
    CHECK(pixel(image, 1, 0)[3] == 0);
    CHECK(pixel(image, 0, 1)[3] == 0);
    const Image hidden = render({2.0, 2.0, {Group{.children = {square}, .opacity = 0.0}}}, 2, 2);
    for (const std::uint8_t byte : hidden.rgba)
        CHECK(byte == 0);
}
