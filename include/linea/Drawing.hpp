// linea - drawings: layers of filled and stroked paths, and groups of them, with clips, masks, blurs and drop shadows,
// rendered to images.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Paint.hpp"
#include "linea/Path.hpp"
#include "linea/Raster.hpp"
#include "linea/Stroke.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace linea {

// Layers and groups take effects, which change a node's own image after it is drawn alone on a transparent surface and
// before it composites over what lies beneath. They apply in the order SVG and CSS apply them: the blur, then the drop
// shadow, which falls from the blurred node, then the clip, which cuts both, then a group's mask, then the opacity. A
// node without effects pays nothing for them: it draws exactly as its opacity alone requires.
//
// A blur convolves the node's image, in premultiplied linear light, with a Gaussian sampled at whole pixels out to
// three standard deviations each way, rounded up to a whole pixel, and scaled to sum to 1, so that it keeps the image's
// total alpha. Cut there, the kernel spreads up to about 1.3% less widely than the whole Gaussian would. Its standard
// deviation is in the node's own units, and the node's transform carries it into pixels: the blur spreads along x and
// along y as a Gaussian of that deviation in the node's coordinates spreads once the transform maps it to the image.
// That is the whole mapped Gaussian unless the transform stretches unevenly along axes tilted from the image's, which
// tilts the Gaussian; the blur then keeps its spread along each image axis but not the tilt.

// How far, in pixels, a blur or a drop shadow reaches at most. A standard deviation counts at most a third of this, so
// that its kernel reaches no farther, and content farther than this beyond the image neither blurs nor casts a shadow
// into it. It bounds the memory and time an effect takes however far a transform scales it.
inline constexpr int effectReach = 512;

// A path that a node shows only inside, as SVG's clipPath element clips. The node's coverage at each pixel is
// multiplied by the path's coverage there, so a clipped node covers the intersection of its shape and the path's;
// where a pixel holds an edge of both, the product of their coverages stands for the area they share. The path is in
// the node's own coordinates, those its transform moves, so the clip moves with the node.
struct Clip {
    Path path{};
    // Which parts of the path's contours the clip covers, as SVG's clip-rule.
    FillRule fillRule = FillRule::nonZero;
    friend bool operator==(const Clip &, const Clip &) = default;
};

// A shadow beneath a node, as CSS's drop-shadow() filter casts one: the node's alpha, blurred, moved and filled with a
// color, composited under the node. The offset and the deviation are in the node's own units, so its transform scales
// and turns them with it. An offset that is not a whole number of pixels moves the shadow by linear interpolation
// between the two nearest, which keeps the shadow's total alpha and moves its centroid exactly.
struct DropShadow {
    // How far the shadow falls from the node.
    Vec2 offset{};
    // The standard deviation of the Gaussian that softens the shadow, as a blur's; 0 for a hard shadow.
    double deviation = 0.0;
    // The shadow's color. Its alpha scales the shadow's.
    Color color{};
    friend constexpr bool operator==(const DropShadow &, const DropShadow &) = default;
};

// One path, filled and then stroked as SVG paints it. Paints use the path's own coordinates, so they move with it.
struct Layer {
    Path path{};
    // The paint inside the path, or nothing for no fill.
    std::optional<Paint> fill{};
    FillRule fillRule = FillRule::nonZero;
    // The paint along the path's outline, or nothing for no stroke.
    std::optional<Paint> stroke{};
    StrokeStyle strokeStyle{};
    // Moves the path, its paints and its stroke into the coordinates of the drawing or group the layer sits in.
    Affine transform{};
    // Scales the alpha of the fill and stroke together, from 0 to 1, as SVG's opacity does, so where the stroke covers
    // the fill, the fill does not show through.
    double opacity = 1.0;
    // The standard deviation, in the path's own units, of a Gaussian blur over the fill and stroke together, as SVG's
    // feGaussianBlur; 0 for none.
    double blur = 0.0;
    // A shadow beneath the fill and stroke, or nothing for none.
    std::optional<DropShadow> shadow{};
    // A path in the layer's own coordinates that the fill and stroke show only inside, or nothing to show them whole.
    std::optional<Clip> clip{};
    friend bool operator==(const Layer &, const Layer &) = default;
};

// What a mask's image gives as the factor that scales the alpha of the group it masks.
enum class MaskMode : std::uint8_t {
    // The mask's alpha.
    alpha,
    // The mask's luminance times its alpha, as SVG's mask element takes by default, computed in linear light: CSS
    // Masking's luminance coefficients, 0.2125 R + 0.7154 G + 0.0721 B, over linear RGB, as for a mask whose
    // color-interpolation is linearRGB. Opaque white gives 1 and opaque black 0.
    luminance,
};

// Layers whose image scales, pixel by pixel, the alpha of the group they mask, as SVG's mask element does. They are
// drawn in order in the group's own coordinates, those its transform moves, and where they draw nothing the group does
// not show. A mask holds layers rather than nodes so that a group can default to no mask: that default is built where
// Group is defined, before Node is complete, and destroying a std::vector of nodes there would need Node complete.
struct Mask {
    std::vector<Layer> layers{};
    MaskMode mode = MaskMode::alpha;
    friend bool operator==(const Mask &, const Mask &) = default;
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
    // The standard deviation, in the group's own units, of a Gaussian blur over the children's image; 0 for none.
    double blur = 0.0;
    // A shadow beneath the children's image, or nothing for none.
    std::optional<DropShadow> shadow{};
    // A path in the group's own coordinates that the children's image shows only inside, or nothing to show it whole.
    std::optional<Clip> clip{};
    // Layers whose image scales the alpha of the children's, or nothing to leave it whole.
    std::optional<Mask> mask{};
    friend bool operator==(const Group &, const Group &) = default;
};

// One layer, or a group of nodes.
struct Node {
    // Implicit, so that a layer or a group stands in a list of nodes as it is: Drawing{width, height, {layer, group}}.
    Node(Layer layer) : content(std::move(layer)) {}
    Node(Group group) : content(std::move(group)) {}

    // The layer or the group itself.
    std::variant<Layer, Group> content;
    friend bool operator==(const Node &, const Node &) = default;
};

// Nodes drawn in order on a canvas @p width by @p height units, x right and y down from the top left.
struct Drawing {
    double width{};
    double height{};
    std::vector<Node> nodes{};
    friend bool operator==(const Drawing &, const Drawing &) = default;
};

// How far, in pixels, a rendered curve or round join strays at most from the true shape.
inline constexpr double renderTolerance = 0.1;

// @p drawing rendered at @p pixelWidth by @p pixelHeight, at least 1 each, its width and height filling the image.
// Nodes composite over one another in linear light, each group as one image. A drawing without area renders
// transparent.
Image render(const Drawing &drawing, int pixelWidth, int pixelHeight);

} // namespace linea
