// linea - images and the coverage rasterizer.
#pragma once

#include "linea/Flatten.hpp"

#include <cstdint>
#include <vector>

namespace linea {

// An RGBA image, row-major from the top left: 4 bytes a pixel, sRGB-encoded and not multiplied by alpha, as PNG files
// store them.
struct Image {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba{};
};

// How overlapping and nested contours fill. Nonzero fills where contours wind around a point at all; even-odd fills
// where they wind around it an odd number of times, so every nested contour cuts a hole.
enum class FillRule : std::uint8_t { nonZero, evenOdd };

// How much of each pixel the closed @p contours cover, from 0 to 1, row-major over @p width by @p height pixels, at
// least 1 each. Contours are in pixel units, x right and y down from the top left corner of the image, and every
// contour is closed whatever its flag says. Coverage is the exact area of each pixel that the polygon covers, with
// the fill rule applied to the winding.
std::vector<float> coverage(const std::vector<Contour> &contours, int width, int height, FillRule rule);

} // namespace linea
