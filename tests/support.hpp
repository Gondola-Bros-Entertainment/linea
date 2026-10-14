// Helpers shared by linea's tests.
#pragma once

#include "linea/Raster.hpp"

#include <numeric>
#include <vector>

namespace linea::test {

inline double total(const std::vector<float> &cover) {
    return std::accumulate(cover.begin(), cover.end(), 0.0,
                           [](double sum, float c) { return sum + static_cast<double>(c); });
}

inline double at(const std::vector<float> &cover, int width, int x, int y) {
    return static_cast<double>(
        cover[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]);
}

} // namespace linea::test
