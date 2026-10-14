// linea - paints: solid colors and linear and radial gradients.
#pragma once

#include "linea/Color.hpp"
#include "linea/Geometry.hpp"

#include <variant>
#include <vector>

namespace linea {

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

} // namespace linea
