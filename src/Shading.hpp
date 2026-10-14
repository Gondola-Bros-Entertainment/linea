// linea - shading paints at points, for the renderer. Internal.
#pragma once

#include "ColorSpace.hpp"
#include "linea/Drawing.hpp"

#include <optional>
#include <vector>

namespace linea::detail {

// A color in linear light with each channel multiplied by alpha, as an image being drawn holds its pixels, so that
// compositing and blending pixels are sums.
struct Pixel {
    double r{};
    double g{};
    double b{};
    double a{};
};

// @p drawing, which has area, drawn as render() draws it at @p width by @p height pixels, at least 1 each, before
// render() encodes it: row-major from the top left. Defined with the renderer, in Drawing.cpp.
std::vector<Pixel> draw(const Drawing &drawing, int width, int height);

// A paint ready to sample: a gradient's stops sorted and converted to its interpolation space, premultiplied, once,
// and a pattern's tile drawn once.
class Shader {
  public:
    // @p paint ready to sample on an image that @p toPixels maps the paint's coordinates onto, which sets the
    // resolution a pattern's tile is drawn at.
    Shader(const Paint &paint, const Affine &toPixels);
    // The paint's color at @p p, in the paint's own coordinates: transparent where it gives none, as outside the cone
    // of a radial gradient whose focal circle lies outside its end circle.
    [[nodiscard]] LinearColor at(Vec2 p) const;

  private:
    struct Stop {
        double offset{};
        // The stop's three coordinates in the interpolation space, each multiplied by alpha, then alpha.
        double x{};
        double y{};
        double z{};
        double alpha{};
    };
    enum class Kind { solid, linear, radial, twoCircle, conic, pattern };

    void prepareStops(const std::vector<GradientStop> &stops, Interpolation interpolation);
    void prepareRadial(const RadialGradient &radial);
    void preparePattern(const Pattern &pattern, const Affine &toPixels);
    [[nodiscard]] std::optional<double> twoCircleOffset(Vec2 p) const;
    [[nodiscard]] LinearColor along(double t) const;
    [[nodiscard]] LinearColor tileAt(Vec2 p) const;

    Kind kind_ = Kind::solid;
    LinearColor solid_{};
    // A linear gradient's start, a radial gradient's center or its focal circle's center, or a conic gradient's
    // center.
    Vec2 from_{};
    // A linear gradient's run from its start to its end, or a radial gradient's from its focal circle's center to its
    // end circle's.
    Vec2 axis_{};
    double axisLength2_{};
    double radius_{};
    // A two-circle radial gradient's focal radius, how much its radius grows from the focal circle to the end circle,
    // and the leading coefficient of the quadratic its offsets solve, which twoCircleOffset() derives.
    double startRadius_{};
    double growth_{};
    double leading_{};
    double startAngle_{};
    SpreadMethod spread_ = SpreadMethod::pad;
    Interpolation interpolation_ = Interpolation::oklab;
    std::vector<Stop> stops_;
    // A pattern's tile, drawn tileWidth_ by tileHeight_ pixels, and the map from the paint's coordinates to the tile's
    // pixels before they wrap around it.
    int tileWidth_{};
    int tileHeight_{};
    std::vector<Pixel> tile_;
    Affine toTile_{};
};

} // namespace linea::detail
