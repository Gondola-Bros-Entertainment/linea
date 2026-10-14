#include "linea/Drawing.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

using namespace linea;

namespace {

const std::uint8_t *pixel(const Image &image, int x, int y) {
    return image.rgba.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
}

Layer filled(Path path, Paint paint) {
    Layer layer;
    layer.path = std::move(path);
    layer.fill = std::move(paint);
    return layer;
}

constexpr Color red{255, 0, 0, 255};
constexpr Color white{255, 255, 255, 255};
constexpr Color black{0, 0, 0, 255};

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
