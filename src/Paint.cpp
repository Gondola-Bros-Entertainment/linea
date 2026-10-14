// linea - Paint implementation: the sRGB transfer function and gradient stops.
#include "linea/Paint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace linea {
namespace {

// The sRGB transfer function's constants, IEC 61966-2-1.
constexpr double linearLimit = 0.04045;
constexpr double encodedLimit = 0.0031308;
constexpr double linearSlope = 12.92;
constexpr double curveOffset = 0.055;
constexpr double curveGamma = 2.4;
constexpr double byteMax = 255.0;

std::array<double, 256> decodeTable() {
    std::array<double, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const double v = static_cast<double>(i) / byteMax;
        table[i] = v <= linearLimit ? v / linearSlope : std::pow((v + curveOffset) / (1.0 + curveOffset), curveGamma);
    }
    return table;
}

} // namespace

double srgbToLinear(std::uint8_t value) {
    static const std::array<double, 256> table = decodeTable();
    return table[value];
}

std::uint8_t linearToSrgb(double value) {
    const double v = std::clamp(value, 0.0, 1.0);
    const double encoded =
        v <= encodedLimit ? v * linearSlope : (1.0 + curveOffset) * std::pow(v, 1.0 / curveGamma) - curveOffset;
    return static_cast<std::uint8_t>(std::lround(encoded * byteMax));
}

std::vector<GradientStop> evenStops(const std::vector<Color> &colors) {
    std::vector<GradientStop> stops;
    for (std::size_t i = 0; i < colors.size(); ++i)
        stops.push_back(
            {colors.size() > 1 ? static_cast<double>(i) / static_cast<double>(colors.size() - 1) : 0.0, colors[i]});
    return stops;
}

} // namespace linea
