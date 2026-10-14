// linea - colors and paints: solid colors and linear and radial gradients.
#pragma once

#include "linea/Geometry.hpp"

#include <cstdint>
#include <variant>
#include <vector>

namespace linea {

// An sRGB color with straight alpha, as images store it.
struct Color {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a = 255;
    friend constexpr bool operator==(Color, Color) = default;
};

// A color that a gradient passes through at @p offset, from 0 at its start to 1 at its end.
struct GradientStop {
    double offset{};
    Color color{};
};

// Colors blend between the stops in Oklab, a perceptual color space, so that a gradient between two saturated colors
// does not pass through grey. Points before the first stop take its color, and points past the last take its color.

// A gradient along the line from @p from to @p to.
struct LinearGradient {
    Vec2 from{};
    Vec2 to{};
    std::vector<GradientStop> stops;
};

// A gradient outward from @p center, reaching its last stop at @p radius.
struct RadialGradient {
    Vec2 center{};
    double radius{};
    std::vector<GradientStop> stops;
};

using Paint = std::variant<Color, LinearGradient, RadialGradient>;

// Stops spread evenly from 0 to 1 through @p colors.
std::vector<GradientStop> evenStops(const std::vector<Color> &colors);

// sRGB-encoded 0 to 255 and linear light 0 to 1, as the sRGB standard defines them.
double srgbToLinear(std::uint8_t value);
std::uint8_t linearToSrgb(double value);

} // namespace linea
