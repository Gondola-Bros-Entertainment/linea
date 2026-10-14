// linea - sRGB, linear light and Oklab, the conversions that colors and gradient shading share. Internal.
#pragma once

#include "linea/Color.hpp"

namespace linea::detail {

// A color in linear light with straight alpha, each from 0 to 1.
struct LinearColor {
    double r{};
    double g{};
    double b{};
    double a{};
};

// The linear light that the sRGB transfer function encodes as @p encoded, from 0 to 1.
double decodeSrgb(double encoded);

// @p c in linear light, its alpha scaled to 0 to 1.
LinearColor linear(Color c);

// @p c in Oklab, alpha carried over.
Oklab linearToOklab(const LinearColor &c);

// @p c in linear light, alpha carried over. A color outside the sRGB gamut has channels outside 0 to 1.
LinearColor oklabToLinear(const Oklab &c);

} // namespace linea::detail
