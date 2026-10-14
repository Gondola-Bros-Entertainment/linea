// Helpers shared by linea's tests.
#pragma once

#include "linea/Drawing.hpp"
#include "linea/Raster.hpp"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace linea::test {

// A quarter turn clockwise, written out because rotate() leaves rounding where its zeros should be, and tests need
// transforms whose products are exact.
inline constexpr Affine quarterTurn{0.0, 1.0, -1.0, 0.0, 0.0, 0.0};

inline double total(const std::vector<float> &cover) {
    return std::accumulate(cover.begin(), cover.end(), 0.0,
                           [](double sum, float c) { return sum + static_cast<double>(c); });
}

inline double at(const std::vector<float> &cover, int width, int x, int y) {
    return static_cast<double>(
        cover[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]);
}

inline const std::uint8_t *pixel(const Image &image, int x, int y) {
    return image.rgba.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
}

inline Color colorAt(const Image &image, int x, int y) {
    const std::uint8_t *p = pixel(image, x, y);
    return {p[0], p[1], p[2], p[3]};
}

inline Layer filled(Path path, Paint paint) {
    Layer layer;
    layer.path = std::move(path);
    layer.fill = std::move(paint);
    return layer;
}

} // namespace linea::test
