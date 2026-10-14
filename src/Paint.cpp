// linea - Paint implementation: gradient stops.
#include "linea/Paint.hpp"

#include <cstddef>

namespace linea {

std::vector<GradientStop> evenStops(const std::vector<Color> &colors) {
    std::vector<GradientStop> stops;
    for (std::size_t i = 0; i < colors.size(); ++i)
        stops.push_back(
            {colors.size() > 1 ? static_cast<double>(i) / static_cast<double>(colors.size() - 1) : 0.0, colors[i]});
    return stops;
}

} // namespace linea
