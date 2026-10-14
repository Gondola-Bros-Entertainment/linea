#include "linea/Drawing.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
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

// @p paint filling a drawing @p width by @p height units, rendered at a pixel a unit, so that pixel (x, y) shows the
// paint at (x + 0.5, y + 0.5).
Image painted(Paint paint, int width, int height) {
    Layer layer;
    layer.path = rectangle({0.0, 0.0}, {static_cast<double>(width), static_cast<double>(height)});
    layer.fill = std::move(paint);
    return render({static_cast<double>(width), static_cast<double>(height), {layer}}, width, height);
}

// White from transparent at offset 0 to opaque at offset 1, so that a pixel's alpha, as a byte, is 255 times the
// offset it shows, rounded.
const std::vector<GradientStop> fade = evenStops({{255, 255, 255, 0}, {255, 255, 255, 255}});

// The alpha bytes of a fade at offsets 0.25 and 0.75: 63.75 and 191.25, rounded.
constexpr std::uint8_t quarter = 64;
constexpr std::uint8_t threeQuarters = 191;

std::uint8_t alphaAt(const Image &image, int x) { return colorAt(image, x, 0).a; }

// Whether @p gradient leaves a drawing 20 units square wholly transparent.
bool blank(const RadialGradient &gradient) {
    const Image image = painted(gradient, 20, 20);
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        if (image.rgba[i] != 0)
            return false;
    return true;
}

} // namespace

TEST_CASE("Repeat and reflect continue a linear gradient past its ends as SVG's spreadMethod does") {
    // The gradient runs from x = 20 to x = 30, so the pixels below show offsets -1.75, -0.75, 0.25, 1.25, 2.75 and
    // 3.75.
    const std::vector<int> xs{2, 12, 22, 32, 47, 57};
    const auto alphas = [&](SpreadMethod spread) {
        const Image image = painted(LinearGradient{{20.0, 0.0}, {30.0, 0.0}, fade, spread}, 60, 1);
        std::vector<std::uint8_t> result;
        for (const int x : xs)
            result.push_back(alphaAt(image, x));
        return result;
    };
    CHECK(alphas(SpreadMethod::pad) == std::vector<std::uint8_t>{0, 0, quarter, 255, 255, 255});
    // Repeat keeps each offset's fraction: 0.25, 0.25, 0.25, 0.25, 0.75 and 0.75.
    CHECK(alphas(SpreadMethod::repeat) ==
          std::vector<std::uint8_t>{quarter, quarter, quarter, quarter, threeQuarters, threeQuarters});
    // Reflect takes each offset's distance from the nearest even number: 0.25, 0.75, 0.25, 0.75, 0.75 and 0.25.
    CHECK(alphas(SpreadMethod::reflect) ==
          std::vector<std::uint8_t>{quarter, threeQuarters, quarter, threeQuarters, threeQuarters, quarter});
}

TEST_CASE("Repeat and reflect continue a radial gradient past its radius as SVG's spreadMethod does") {
    // The gradient is 10 across about the left end of the row, so the pixels below show offsets 0.25, 1.25, 2.75 and
    // 3.75.
    const std::vector<int> xs{2, 12, 27, 37};
    const auto alphas = [&](SpreadMethod spread) {
        const Image image =
            painted(RadialGradient{.center = {0.0, 0.5}, .radius = 10.0, .stops = fade, .spread = spread}, 40, 1);
        std::vector<std::uint8_t> result;
        for (const int x : xs)
            result.push_back(alphaAt(image, x));
        return result;
    };
    CHECK(alphas(SpreadMethod::pad) == std::vector<std::uint8_t>{quarter, 255, 255, 255});
    CHECK(alphas(SpreadMethod::repeat) == std::vector<std::uint8_t>{quarter, quarter, threeQuarters, threeQuarters});
    CHECK(alphas(SpreadMethod::reflect) == std::vector<std::uint8_t>{quarter, threeQuarters, threeQuarters, quarter});
}

namespace {

// A circle of the canvas's radial gradient.
struct Circle {
    Vec2 center{};
    double radius{};
};

// What the canvas's definition gives a point.
struct CanvasOffset {
    // Whether the point lies within rounding of the edge of the cone the circles sweep, where either answer is right.
    bool nearEdge = false;
    // The point's offset, or nothing when no circle passes through it.
    std::optional<double> offset{};
};

// The offset that the HTML canvas's createRadialGradient() from @p start to @p end gives @p p, read straight from the
// canvas's definition: of the circles c(t) = start + t (end - start), the one through @p p with the greatest t among
// those whose radius is greater than 0. That condition squares to A t^2 + B t + C = 0, solved here by the textbook
// formula.
CanvasOffset canvasOffset(Vec2 p, Circle start, Circle end) {
    // How close to 0 the discriminant, relative to the terms it sums, can come before rounding could flip its sign.
    constexpr double edge = 1e-9;
    const Vec2 d = end.center - start.center;
    const Vec2 q = p - start.center;
    const double g = end.radius - start.radius;
    const double a = dot(d, d) - g * g;
    const double b = -2.0 * (dot(q, d) + start.radius * g);
    const double c = dot(q, q) - start.radius * start.radius;
    std::vector<double> roots;
    if (a == 0.0) {
        if (b != 0.0)
            roots.push_back(-c / b);
    } else {
        const double discriminant = b * b - 4.0 * a * c;
        if (std::abs(discriminant) <= edge * (b * b + std::abs(4.0 * a * c)))
            return {.nearEdge = true};
        if (discriminant > 0.0) {
            roots.push_back((-b + std::sqrt(discriminant)) / (2.0 * a));
            roots.push_back((-b - std::sqrt(discriminant)) / (2.0 * a));
        }
    }
    CanvasOffset result;
    for (const double t : roots)
        if (start.radius + t * g > 0.0 && (!result.offset || t > *result.offset))
            result.offset = t;
    return result;
}

} // namespace

TEST_CASE("A radial gradient with a focal circle gives each point the offset of the canvas's definition") {
    // From alpha 0.2 at offset 0 to 1 at offset 1, repeating, so that every point a circle passes through shows, and
    // its alpha gives its offset's fraction to within half a byte's step, 1 / 408, measured around the period.
    const std::vector<GradientStop> faint = evenStops({{255, 255, 255, 51}, {255, 255, 255, 255}});
    constexpr double low = 0.2;
    constexpr double span = 0.8;
    constexpr double step = 0.5 / (255.0 * span);
    constexpr double rounding = 1e-9;
    constexpr int size = 60;
    struct Case {
        const char *name;
        Circle focal;
        Circle end;
        // Whether the circles sweep only a cone, a band or a half-plane, leaving the rest of the image bare.
        bool leavesBare;
    };
    const std::vector<Case> cases{
        {"a focal point inside the end circle", {{20.0, 30.5}, 0.0}, {{30.0, 30.5}, 25.0}, false},
        {"a focal circle inside the end circle, off its center", {{24.0, 27.0}, 6.0}, {{31.0, 33.0}, 24.0}, false},
        {"concentric circles", {{30.0, 30.0}, 9.0}, {{30.0, 30.0}, 23.0}, false},
        {"a focal circle around the end circle", {{30.0, 30.0}, 28.0}, {{34.0, 28.0}, 6.0}, false},
        {"a focal point on the end circle, which leaves a half-plane", {{50.0, 30.0}, 0.0}, {{30.0, 30.0}, 20.0}, true},
        {"a focal point outside the end circle, which leaves a cone", {{5.0, 30.0}, 0.0}, {{40.0, 30.0}, 15.0}, true},
        {"circles of equal radius, which leave a band", {{15.0, 31.0}, 8.0}, {{40.0, 27.0}, 8.0}, true},
        {"a focal circle that crosses the end circle, which leaves a cone",
         {{18.0, 22.0}, 12.0},
         {{38.0, 36.0}, 16.0},
         true},
    };
    for (const Case &c : cases) {
        CAPTURE(c.name);
        const Image image = painted(RadialGradient{.center = c.end.center,
                                                   .radius = c.end.radius,
                                                   .stops = faint,
                                                   .focal = c.focal.center,
                                                   .focalRadius = c.focal.radius,
                                                   .spread = SpreadMethod::repeat},
                                    size, size);
        int shown = 0;
        int bare = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const CanvasOffset expected = canvasOffset({x + 0.5, y + 0.5}, c.focal, c.end);
                if (expected.nearEdge)
                    continue;
                CAPTURE(x);
                CAPTURE(y);
                const double alpha = colorAt(image, x, y).a / 255.0;
                if (!expected.offset) {
                    CHECK(alpha == 0.0);
                    ++bare;
                    continue;
                }
                ++shown;
                const double t = *expected.offset;
                CAPTURE(t);
                const double apart = std::abs((alpha - low) / span - (t - std::floor(t)));
                CHECK(std::min(apart, 1.0 - apart) <= step + rounding);
            }
        }
        // The checks above cover a good part of the image, shown and, where the circles leave some bare, bare.
        CHECK(shown > size * size / 8);
        CHECK((bare > size * size / 8) == c.leavesBare);
    }
}

TEST_CASE("A radial gradient paints nothing where the canvas paints nothing") {
    const std::vector<GradientStop> stops = evenStops({{255, 0, 0, 255}, {0, 0, 255, 255}});
    // A focal circle that is the end circle.
    CHECK(blank({.center = {10.0, 10.0}, .radius = 6.0, .stops = stops, .focalRadius = 6.0}));
    // A focal point on the end circle leaves bare the half-plane beyond the circle's tangent there, x > 16, and a focal
    // point outside it everything outside the cone.
    const Image half = painted(
        RadialGradient{.center = {10.0, 10.0}, .radius = 6.0, .stops = stops, .focal = Vec2{16.0, 10.0}}, 20, 20);
    CHECK(colorAt(half, 17, 3).a == 0);
    CHECK(colorAt(half, 15, 3).a == 255);
    const Image cone = painted(
        RadialGradient{.center = {14.0, 10.0}, .radius = 3.0, .stops = stops, .focal = Vec2{2.0, 10.0}}, 20, 20);
    CHECK(colorAt(cone, 1, 10).a == 0);
    CHECK(colorAt(cone, 10, 2).a == 0);
    CHECK(colorAt(cone, 10, 10).a == 255);
}

TEST_CASE("A radial gradient of radius 0 paints its last stop everywhere, whatever the spread") {
    // The last stop is listed first and lies short of offset 1, so it is last only once the stops sort.
    const Color first{200, 40, 40, 255};
    const Color last{40, 40, 200, 255};
    const std::vector<GradientStop> stops{{0.6, last}, {0.0, first}};
    for (const SpreadMethod spread : {SpreadMethod::pad, SpreadMethod::repeat, SpreadMethod::reflect}) {
        CAPTURE(static_cast<int>(spread));
        // Centered on a pixel's center, so that the center itself is among the points painted.
        const Image image =
            painted(RadialGradient{.center = {10.5, 10.5}, .radius = 0.0, .stops = stops, .spread = spread}, 20, 20);
        int other = 0;
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x)
                if (colorAt(image, x, y) != last)
                    ++other;
        CHECK(other == 0);
    }
}

TEST_CASE("A radial gradient with a negative or non-finite radius or focal radius paints nothing") {
    constexpr double infinity = std::numeric_limits<double>::infinity();
    const std::vector<GradientStop> stops = evenStops({{255, 0, 0, 255}, {0, 0, 255, 255}});
    for (const double bad : {-6.0, infinity, -infinity, std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(bad);
        // With the focal circle at its default, the point at the center, and with a focal point elsewhere.
        CHECK(blank({.center = {10.0, 10.0}, .radius = bad, .stops = stops}));
        CHECK(blank({.center = {10.0, 10.0}, .radius = bad, .stops = stops, .focal = Vec2{8.0, 10.0}}));
        CHECK(blank({.center = {10.0, 10.0}, .radius = 6.0, .stops = stops, .focalRadius = bad}));
        CHECK(blank(
            {.center = {10.0, 10.0}, .radius = 6.0, .stops = stops, .focal = Vec2{8.0, 10.0}, .focalRadius = bad}));
    }
}

TEST_CASE("A conic gradient takes each stop's color at its angle all the way around, and meets itself at the start") {
    // Nine stops an eighth of a turn apart, the last unlike the first, starting straight up as CSS's 0deg does and
    // turning clockwise. Pixel centers 20 units from the center in each of the eight directions lie exactly on a stop.
    const std::vector<Color> colors{{230, 25, 75, 255},  {245, 130, 48, 255}, {255, 225, 25, 255},
                                    {60, 180, 75, 255},  {70, 240, 240, 255}, {0, 130, 200, 255},
                                    {145, 30, 180, 255}, {240, 50, 230, 255}, {128, 128, 128, 255}};
    const Image image = painted(ConicGradient{{30.5, 30.5}, -std::numbers::pi / 2.0, evenStops(colors)}, 61, 61);
    // Straight up, then clockwise.
    const std::vector<std::pair<int, int>> directions{{0, -1}, {1, -1}, {1, 0},  {1, 1},
                                                      {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
    for (std::size_t k = 0; k < directions.size(); ++k) {
        CAPTURE(k);
        CHECK(colorAt(image, 30 + 20 * directions[k].first, 30 + 20 * directions[k].second) == colors[k]);
    }
    // The center, in no direction, takes the offset of +x, a quarter turn from the start.
    CHECK(colorAt(image, 30, 30) == colors[2]);

    // Two halves with a hard edge between them: the pixel on the start angle and the one just clockwise of it take the
    // first half's color, and the one just short of it the second half's.
    const Color first{200, 40, 40, 255};
    const Color second{40, 40, 200, 255};
    const Image halves = painted(ConicGradient{{30.5, 30.5},
                                               -std::numbers::pi / 2.0,
                                               {{0.0, first}, {0.5, first}, {0.5, second}, {1.0, second}}},
                                 61, 61);
    CHECK(colorAt(halves, 30, 2) == first);
    CHECK(colorAt(halves, 31, 2) == first);
    CHECK(colorAt(halves, 29, 2) == second);
    // Straight down, half a turn on, the second half begins.
    CHECK(colorAt(halves, 30, 58) == second);
    CHECK(colorAt(halves, 31, 58) == first);
}

TEST_CASE("Gradients blend in the interpolation space they name, as CSS Color 4 computes") {
    // Halfway from black to (200, 100, 0), at pixel 1 of a gradient from x = 0.5 to x = 2.5. The values are worked out
    // independently from the definitions of sRGB and Oklab, and lie at least 0.1 from a rounding boundary.
    const auto halfway = [](Interpolation interpolation) {
        const Image image = painted(LinearGradient{.from = {0.5, 0.0},
                                                   .to = {2.5, 0.0},
                                                   .stops = evenStops({{0, 0, 0, 255}, {200, 100, 0, 255}}),
                                                   .interpolation = interpolation},
                                    3, 1);
        return colorAt(image, 1, 0);
    };
    // Oklab: lightness halfway, 75.96, 33.92 and 0 in sRGB.
    CHECK(halfway(Interpolation::oklab) == Color{76, 34, 0, 255});
    // sRGB's encoded channels: exactly half of each.
    CHECK(halfway(Interpolation::srgb) == Color{100, 50, 0, 255});
    // Linear light: half of each channel's light, 146.31, 71.40 and 0 once encoded.
    CHECK(halfway(Interpolation::srgbLinear) == Color{146, 71, 0, 255});
}

TEST_CASE("Every interpolation space blends premultiplied, so a transparent stop lends no color") {
    // A gradient from x = 0.5 to x = 4.5, so that pixel 1 shows offset 0.25 and pixel 2 offset 0.5.
    const auto blend = [](Color from, Color to, Interpolation interpolation) {
        return painted(
            LinearGradient{
                .from = {0.5, 0.0}, .to = {4.5, 0.0}, .stops = evenStops({from, to}), .interpolation = interpolation},
            5, 1);
    };
    const Color red{255, 0, 0, 255};
    const Color clearBlue{0, 0, 255, 0};
    const Color fadedBlue{0, 0, 255, 153};
    for (const Interpolation interpolation : {Interpolation::oklab, Interpolation::srgb, Interpolation::srgbLinear}) {
        CAPTURE(static_cast<int>(interpolation));
        // A quarter of the way to transparent blue is pure red at alpha 0.75, where blending straight colors would mix
        // in a quarter of blue.
        CHECK(colorAt(blend(red, clearBlue, interpolation), 1, 0) == Color{255, 0, 0, 191});
    }
    // Halfway from red to blue at alpha 0.6 is alpha 0.8, with each coordinate times alpha halfway, so that blue weighs
    // 0.3 against red's 0.5. Worked out independently, at least 0.1 from a rounding boundary: sRGB 159.375 and 95.625,
    // linear light 207.15 and 164.75 once encoded, and Oklab 169.24, 80.98 and 136.94.
    CHECK(colorAt(blend(red, fadedBlue, Interpolation::srgb), 2, 0) == Color{159, 0, 96, 204});
    CHECK(colorAt(blend(red, fadedBlue, Interpolation::srgbLinear), 2, 0) == Color{207, 0, 165, 204});
    CHECK(colorAt(blend(red, fadedBlue, Interpolation::oklab), 2, 0) == Color{169, 81, 137, 204});
}
