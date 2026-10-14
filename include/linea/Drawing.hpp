// linea - drawings: layers of filled and stroked paths, and groups of them, with clips, masks, blurs and drop shadows,
// rendered to images.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Indirect.hpp"
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
// A blur spreads the node's image, in premultiplied linear light, as a Gaussian does, along x and then along y. Its
// standard deviation is in the node's own units, and the node's transform carries it into pixels: the blur spreads
// along x and along y as a Gaussian of that deviation in the node's coordinates spreads once the transform maps it to
// the image. That is the whole mapped Gaussian unless the transform stretches unevenly along axes tilted from the
// image's, which tilts the Gaussian; the blur then keeps its spread along each image axis but not the tilt.
//
// Along an axis where the deviation is less than boxBlurDeviation pixels, the blur convolves with the Gaussian sampled
// at whole pixels out to three standard deviations each way, rounded up to a whole pixel, and scaled to sum to 1. Cut
// there, the kernel spreads up to about 1.3% less widely than the whole Gaussian would. From boxBlurDeviation up, three
// box blurs stand in for it, as SVG's feGaussianBlur allows, in time that does not grow with the deviation. Each
// averages a whole number of pixels and a fraction of the pixel beyond each end, sized so that the three spread with
// exactly the deviation asked for (extended box filtering, after Gwosdek, Grewenig, Bruhn and Weickert, 2011), up to
// about 170.5 pixels: there the boxes are the widest that stay within effectReach, and up to the cap of a third of it
// they spread at most 0.1% less. They reach from two pixels less far than the kernel would to one pixel farther, and a
// pixel's alpha differs from what the kernel would give it by at most 2.7 of 255 levels across a straight edge, and by
// at most 10 levels, 3.9%, whatever the image. Either way every pixel read weighs a positive amount, and the image
// keeps its total alpha.

// How far, in pixels, a blur or a drop shadow reaches at most. A standard deviation counts at most a third of this, so
// that its kernel or its boxes reach no farther, and content farther than this beyond the image neither blurs nor casts
// a shadow into it. It bounds the memory and time an effect takes however far a transform scales it.
inline constexpr int effectReach = 512;

// The standard deviation, in pixels along an axis, from which a blur or a drop shadow spreads by box blurs rather than
// by the sampled Gaussian: where the two take the same time. Below it the kernel is the faster, and it is exact.
inline constexpr double boxBlurDeviation = 4.0;

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

// Defined after Node and Mask, which hold groups: a node holds its group through an Indirect, which accepts a type not
// yet complete.
struct Group;

// One layer, or a group of nodes. A node holds its group in storage of its own, so that a group can hold nodes, and
// copies and compares it as a value all the same.
struct Node {
    // Implicit, so that a layer or a group stands in a list of nodes as it is: Drawing{width, height, {layer, group}}.
    // Defined below Group, because a constructor may destroy the content it builds, and destroying a group needs Group
    // complete.
    Node(Layer layer);
    Node(Group group);

    // The layer, or the group.
    std::variant<Layer, Indirect<Group>> content;
    friend bool operator==(const Node &, const Node &) = default;
};

// Nodes whose image scales, pixel by pixel, the alpha of the group they mask, as SVG's mask element does. They are
// drawn in order in the group's own coordinates, those its transform moves, as its children are, and where they draw
// nothing the group does not show.
struct Mask {
    std::vector<Node> nodes{};
    MaskMode mode = MaskMode::alpha;
    friend bool operator==(const Mask &, const Mask &) = default;
};

// Nodes that move and fade together, so that a part built once can be placed many times. A group draws its children on
// a transparent image of its own and composites that image over what lies beneath, as SVG draws a g element, so
// children that overlap in a translucent group do not show through one another.
struct Group {
    // Drawn in order, each over the ones before it.
    std::vector<Node> children{};
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
    // Nodes whose image scales the alpha of the children's, or nothing to leave it whole.
    std::optional<Mask> mask{};
    friend bool operator==(const Group &, const Group &) = default;
};

inline Node::Node(Layer layer) : content(std::move(layer)) {}
inline Node::Node(Group group) : content(Indirect<Group>(std::move(group))) {}

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
