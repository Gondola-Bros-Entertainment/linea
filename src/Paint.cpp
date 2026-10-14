// linea - Paint implementation: gradient stops, and pattern equality.
#include "linea/Paint.hpp"

#include "linea/Drawing.hpp"

#include <cstddef>

namespace linea {

std::vector<GradientStop> evenStops(const std::vector<Color> &colors) {
    std::vector<GradientStop> stops;
    for (std::size_t i = 0; i < colors.size(); ++i)
        stops.push_back(
            {colors.size() > 1 ? static_cast<double>(i) / static_cast<double>(colors.size() - 1) : 0.0, colors[i]});
    return stops;
}

bool operator==(const Pattern &left, const Pattern &right) {
    if (left.transform != right.transform || (left.tile == nullptr) != (right.tile == nullptr))
        return false;
    return left.tile == right.tile || *left.tile == *right.tile;
}

} // namespace linea
