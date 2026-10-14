// linea - paints: solid colors; linear, radial and conic gradients, how they continue past their ends and the color
// space they blend in; and patterns that repeat a drawing.
#pragma once

#include "linea/Color.hpp"
#include "linea/Geometry.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace linea {

// A color that a gradient passes through at @p offset, from 0 at its start to 1 at its end.
struct GradientStop {
    double offset{};
    Color color{};
};

// Each gradient below gives every point an offset, and its stops give each offset a color. Stops sort by offset, each
// clamped to 0 to 1, and stops at one offset keep their order, so that two of them make a hard edge. An offset before
// the first stop takes its color, one past the last stop takes its color, and one between two stops blends theirs in
// the gradient's interpolation space.

// The color space a gradient blends its stops in, as CSS Color 4 names its interpolation spaces. Every space blends
// premultiplied, as CSS Images 4 specifies: alpha, and each coordinate times alpha, move in proportion to the offset,
// and the color is that blend divided by its alpha, so that a transparent stop lends no color.
enum class Interpolation : std::uint8_t {
    // Oklab, as CSS's "in oklab", where equal distances look like equal differences, so that a blend between two
    // saturated colors does not pass through grey. A blend that strays outside the sRGB gamut clips back to it.
    oklab,
    // sRGB's encoded channels, each from 0 to 1, as CSS's "in srgb", and as SVG and CSS blend gradients by default.
    srgb,
    // sRGB's channels in linear light, as CSS's "in srgb-linear", and as SVG's color-interpolation of linearRGB.
    srgbLinear,
};

// How a gradient continues where its offset runs below 0 or past 1, as SVG's spreadMethod.
enum class SpreadMethod : std::uint8_t {
    // Each end's color carries on outward.
    pad,
    // The gradient starts over at each end: offset t takes the color at t minus its floor.
    repeat,
    // The gradient runs back and forth: offset t takes the color at its distance from the nearest even whole number,
    // so that each period runs the other way from the one before, and the two meet at the color of 0 or of 1.
    reflect,
};

// A gradient along the line from @p from to @p to: a point's offset is how far along that line it projects, 0 at
// @p from and 1 at @p to. When the two coincide, every point takes offset 0.
struct LinearGradient {
    Vec2 from{};
    Vec2 to{};
    std::vector<GradientStop> stops;
    SpreadMethod spread = SpreadMethod::pad;
    Interpolation interpolation = Interpolation::oklab;
};

// A gradient from a focal circle out to the circle of @p radius about @p center, as SVG's radialGradient element with
// its fx, fy and fr. With the focal circle at its default, the point @p center, a point's offset is its distance from
// @p center divided by @p radius. A radius of 0 paints every point the last stop's color, whatever the spread, as SVG
// specifies and as the gradient tends to as its radius shrinks, every point but the center passing offset 1.
//
// Any other focal circle makes the gradient the HTML canvas's createRadialGradient(), which SVG 2 adopts: for every
// real t, the circle whose center and radius lie t of the way from the focal circle's to the end circle's, extrapolated
// past both, holds offset t, and a point takes the offset of the circle through it with the greatest t, among those of
// radius 0 or more. Where one circle lies inside the other, every point has an offset. Elsewhere the circles sweep out
// a cone that touches both, a band when their radii are equal, or a half-plane when one touches the other from inside,
// and a point outside it has no offset and stays transparent. A focal circle that is the end circle itself, which the
// canvas leaves unpainted, paints nothing. The canvas paints only circles of radius greater than 0; counting radius 0
// too gives an offset to the one point where the circles shrink to nothing, such as a focal point inside the end
// circle, which takes offset 0 rather than being left a hole.
//
// Whatever the focal circle, a negative radius or focal radius, which the canvas rejects, or one that is not finite,
// paints nothing.
struct RadialGradient {
    Vec2 center{};
    double radius{};
    std::vector<GradientStop> stops;
    // The focal circle's center, where offset 0 lies, or nothing for @p center.
    std::optional<Vec2> focal{};
    // The focal circle's radius.
    double focalRadius = 0.0;
    SpreadMethod spread = SpreadMethod::pad;
    Interpolation interpolation = Interpolation::oklab;
};

// A gradient around @p center, as CSS's conic-gradient(): a point's offset is the angle, clockwise, from
// @p startAngle to the point's direction from @p center, as a fraction of a full turn, from 0 up to 1. Angles are
// radians clockwise from +x on a y-down canvas, as the HTML canvas's createConicGradient() measures them, so CSS's
// "from 0deg", straight up, is a start angle of -pi / 2. The gradient meets itself at the start angle: just short of it
// the color is that of offset 1, and from it on that of offset 0. The center itself, which lies in no direction, takes
// the offset of the direction +x.
struct ConicGradient {
    Vec2 center{};
    double startAngle{};
    std::vector<GradientStop> stops;
    Interpolation interpolation = Interpolation::oklab;
};

// Defined in Drawing.hpp, whose layers hold paints. A pattern holds the drawing it repeats through a pointer, so that a
// pattern can be copied and destroyed where Drawing, and the nodes it holds, are not yet complete.
struct Drawing;

// A paint that repeats a drawing edge to edge across the plane, as SVG's pattern element repeats its content. The
// tile, @p tile's width by height units from its top left corner at the origin, with its nodes cut to that rectangle,
// repeats at every whole multiple of its width along x and of its height along y. @p transform then moves the whole
// grid of tiles into the coordinates of the path it paints, as SVG's patternTransform. A pattern without a tile, a tile
// without area or without nodes, or a transform with no inverse paints nothing. Copies of a pattern share its tile,
// which nothing changes once it is made.
//
// Each time a layer draws with a pattern, the tile is drawn once on an image of its own, as render() draws it, at the
// resolution it has on the image being drawn: each of its sides spans the whole number of pixels nearest that side's
// length once the transforms map it onto the image, from 1 to maxTileSide. The pattern samples that image bilinearly,
// in linear light with premultiplied alpha, wrapping around its edges so that tiles meet without seams; where the
// tile's pixels land squarely on the image's, as at whole scales with no turn, each pixel takes its tile pixel's color
// exactly.
struct Pattern {
    std::shared_ptr<const Drawing> tile;
    Affine transform{};
};

// The most pixels a pattern's tile spans along either of its sides when it is drawn for sampling. A tile whose side
// spans more of the image is drawn at this many pixels along it and stretched to fit, which bounds the memory and time
// a tile takes to those of drawing a 2048 by 2048 image, however far a transform scales it.
inline constexpr int maxTileSide = 2048;

// What fills or strokes a path. Every paint is in the coordinates of the path it paints, so it moves with the path.
using Paint = std::variant<Color, LinearGradient, RadialGradient, ConicGradient, Pattern>;

// Stops spread evenly from 0 to 1 through @p colors.
std::vector<GradientStop> evenStops(const std::vector<Color> &colors);

} // namespace linea
