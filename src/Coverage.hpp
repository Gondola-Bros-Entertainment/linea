// linea - coverage over only the pixels a shape reaches, for the renderer. Internal.
#pragma once

#include "linea/Raster.hpp"

#include <cstddef>
#include <vector>

namespace linea::detail {

// A rectangle of whole pixels: columns from x0 up to but not including x1, and rows from y0 up to but not including
// y1. It holds no pixels when either range is empty.
struct PixelBox {
    int x0{};
    int y0{};
    int x1{};
    int y1{};

    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] int width() const { return x1 - x0; }
    [[nodiscard]] int height() const { return y1 - y0; }
    // Where pixel (@p x, @p y), which the box holds, lies in a row-major array of the box's pixels.
    [[nodiscard]] std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(width()) + static_cast<std::size_t>(x - x0);
    }
};

// The pixels both @p a and @p b hold.
PixelBox intersection(PixelBox a, PixelBox b);

// The smallest box that holds every pixel of @p a and of @p b.
PixelBox hull(PixelBox a, PixelBox b);

// The pixels of an image @p width by @p height that the box around @p contours overlaps, those that coverage() can
// cover: it gives exactly 0 everywhere else. Contours that coverage() passes over do not count.
PixelBox pixelBounds(const std::vector<Contour> &contours, int width, int height);

// Coverage of the pixels of a box, row-major.
struct BoxCoverage {
    PixelBox box;
    std::vector<float> values;
};

// What coverage() gives over the pixels that pixelBounds() gives, in time and memory that grow with those pixels
// rather than with the image.
BoxCoverage boxCoverage(const std::vector<Contour> &contours, int width, int height, FillRule rule);

} // namespace linea::detail
