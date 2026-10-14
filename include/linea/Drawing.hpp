// linea - drawings: layers of filled and stroked paths, and groups of them, rendered to images.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Paint.hpp"
#include "linea/Path.hpp"
#include "linea/Raster.hpp"
#include "linea/Stroke.hpp"

#include <optional>
#include <utility>
#include <variant>
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
    // Moves the path, its paints and its stroke into the coordinates of the drawing or group the layer sits in.
    Affine transform{};
    // Scales the alpha of the fill and stroke together, from 0 to 1, as SVG's opacity does, so where the stroke covers
    // the fill, the fill does not show through.
    double opacity = 1.0;
};

// Declared ahead so that a group can hold nodes that hold groups: std::vector, unlike std::variant, accepts an element
// type that is not yet complete.
struct Node;

// Nodes that move and fade together, so that a part built once can be placed many times. A group draws its children on
// a transparent image of its own and composites that image over what lies beneath, as SVG draws a g element, so
// children that overlap in a translucent group do not show through one another.
struct Group {
    // Drawn in order, each over the ones before it.
    std::vector<Node> children;
    // Moves the children into the coordinates the group sits in, after each child's own transform.
    Affine transform{};
    // Scales the alpha of the children's image as a whole, from 0 to 1.
    double opacity = 1.0;
};

// One layer, or a group of nodes.
struct Node {
    // Implicit, so that a layer or a group stands in a list of nodes as it is: Drawing{width, height, {layer, group}}.
    Node(Layer layer) : content(std::move(layer)) {}
    Node(Group group) : content(std::move(group)) {}

    // The layer or the group itself.
    std::variant<Layer, Group> content;
};

// Nodes drawn in order on a canvas @p width by @p height units, x right and y down from the top left.
struct Drawing {
    double width{};
    double height{};
    std::vector<Node> nodes;
};

// How far, in pixels, a rendered curve or round join strays at most from the true shape.
inline constexpr double renderTolerance = 0.1;

// @p drawing rendered at @p pixelWidth by @p pixelHeight, at least 1 each, its width and height filling the image.
// Nodes composite over one another in linear light, each group as one image. A drawing without area renders
// transparent.
Image render(const Drawing &drawing, int pixelWidth, int pixelHeight);

} // namespace linea
