// linea - drawings: layers of filled and stroked paths, rendered to images.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Paint.hpp"
#include "linea/Path.hpp"
#include "linea/Raster.hpp"
#include "linea/Stroke.hpp"

#include <optional>
#include <vector>

namespace linea {

// One path, filled and then stroked as SVG paints it. Paints use the path's own coordinates, so they move with it.
struct Layer {
    Path path;
    // The paint inside the path, or nothing for no fill.
    std::optional<Paint> fill;
    FillRule fillRule = FillRule::nonZero;
    // The paint along the path's outline, or nothing for no stroke.
    std::optional<Paint> stroke;
    StrokeStyle strokeStyle{};
    // Moves the path, its paints and its stroke into the drawing's coordinates.
    Affine transform{};
    // Scales the layer's alpha, from 0 to 1.
    double opacity = 1.0;
};

// Layers drawn in order on a canvas @p width by @p height units, x right and y down from the top left.
struct Drawing {
    double width{};
    double height{};
    std::vector<Layer> layers;
};

// How far, in pixels, a rendered curve or round join strays at most from the true shape.
inline constexpr double renderTolerance = 0.1;

// @p drawing rendered at @p pixelWidth by @p pixelHeight, at least 1 each, its width and height filling the image.
// Layers composite over one another in linear light. A drawing without area renders transparent.
Image render(const Drawing &drawing, int pixelWidth, int pixelHeight);

} // namespace linea
