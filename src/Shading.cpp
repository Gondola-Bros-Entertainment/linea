// linea - Shading implementation: gradients blended premultiplied in their interpolation spaces, and pattern tiles
// sampled bilinearly in premultiplied linear light.
#include "Shading.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <variant>

namespace linea::detail {
namespace {

constexpr double fullTurn = 2.0 * std::numbers::pi;
// The largest byte, which an sRGB channel's encoded value is a fraction of.
constexpr double byteMax = 255.0;

// @p t moved into 0 to 1 as @p spread continues a gradient past its ends. Pad leaves it as it is, for the end stops to
// carry on past.
double spreadOffset(double t, SpreadMethod spread) {
    switch (spread) {
    case SpreadMethod::pad:
        return t;
    case SpreadMethod::repeat:
        return t - std::floor(t);
    case SpreadMethod::reflect: {
        // Reflecting is symmetric about 0 and repeats every 2, and fmod is exact.
        const double folded = std::fmod(std::abs(t), 2.0);
        return folded > 1.0 ? 2.0 - folded : folded;
    }
    }
    return t;
}

// The whole number of pixels nearest @p extent, from 1 to maxTileSide. fmax and fmin pass over an extent that is not a
// number.
int tilePixels(double extent) {
    return static_cast<int>(std::lround(std::fmin(std::fmax(extent, 1.0), static_cast<double>(maxTileSide))));
}

// The whole number @p i wrapped onto 0 to @p count - 1. fmod is exact, so it wraps whole numbers of any size exactly.
std::size_t wrapped(double i, int count) {
    const auto n = static_cast<double>(count);
    const double r = std::fmod(i, n);
    return static_cast<std::size_t>(r < 0.0 ? r + n : r);
}

} // namespace

Shader::Shader(const Paint &paint, const Affine &toPixels) {
    if (const auto *color = std::get_if<Color>(&paint)) {
        solid_ = linear(*color);
    } else if (const auto *gradient = std::get_if<LinearGradient>(&paint)) {
        kind_ = Kind::linear;
        from_ = gradient->from;
        axis_ = gradient->to - gradient->from;
        axisLength2_ = dot(axis_, axis_);
        spread_ = gradient->spread;
        prepareStops(gradient->stops, gradient->interpolation);
    } else if (const auto *radial = std::get_if<RadialGradient>(&paint)) {
        prepareRadial(*radial);
    } else if (const auto *conic = std::get_if<ConicGradient>(&paint)) {
        kind_ = Kind::conic;
        from_ = conic->center;
        startAngle_ = conic->startAngle;
        prepareStops(conic->stops, conic->interpolation);
    } else if (const auto *pattern = std::get_if<Pattern>(&paint)) {
        preparePattern(*pattern, toPixels);
    }
}

void Shader::prepareStops(const std::vector<GradientStop> &stops, Interpolation interpolation) {
    interpolation_ = interpolation;
    for (const GradientStop &stop : stops) {
        const double offset = std::clamp(stop.offset, 0.0, 1.0);
        const LinearColor light = linear(stop.color);
        switch (interpolation) {
        case Interpolation::oklab: {
            const Oklab lab = linearToOklab(light);
            stops_.push_back({offset, lab.l * lab.alpha, lab.a * lab.alpha, lab.b * lab.alpha, lab.alpha});
            break;
        }
        case Interpolation::srgb:
            stops_.push_back({offset, stop.color.r / byteMax * light.a, stop.color.g / byteMax * light.a,
                              stop.color.b / byteMax * light.a, light.a});
            break;
        case Interpolation::srgbLinear:
            stops_.push_back({offset, light.r * light.a, light.g * light.a, light.b * light.a, light.a});
            break;
        }
    }
    std::stable_sort(stops_.begin(), stops_.end(),
                     [](const Stop &left, const Stop &right) { return left.offset < right.offset; });
}

void Shader::prepareRadial(const RadialGradient &radial) {
    // A negative radius, which the canvas rejects, or one that is not finite paints nothing: the paint stays
    // transparent.
    const auto usable = [](double radius) { return std::isfinite(radius) && radius >= 0.0; };
    if (!usable(radial.radius) || !usable(radial.focalRadius))
        return;
    spread_ = radial.spread;
    prepareStops(radial.stops, radial.interpolation);
    const Vec2 focal = radial.focal.value_or(radial.center);
    if (focal != radial.center || radial.focalRadius != 0.0) {
        kind_ = Kind::twoCircle;
        from_ = focal;
        axis_ = radial.center - focal;
        startRadius_ = radial.focalRadius;
        growth_ = radial.radius - radial.focalRadius;
        leading_ = dot(axis_, axis_) - growth_ * growth_;
    } else if (radial.radius > 0.0) {
        kind_ = Kind::radial;
        from_ = radial.center;
        radius_ = radial.radius;
    } else {
        // As the radius shrinks to 0, every point but the center passes offset 1, so the gradient tends to its last
        // stop's color, which SVG paints for a radius of 0 whatever the spread. Offsets clamp to 1 at most, so the
        // color at offset 1 is the last stop's.
        solid_ = along(1.0);
    }
}

void Shader::preparePattern(const Pattern &pattern, const Affine &toPixels) {
    const double det = determinant(pattern.transform);
    if (!pattern.tile || det == 0.0 || !std::isfinite(det))
        return;
    const Drawing &tile = *pattern.tile;
    if (!(tile.width > 0.0) || !(tile.height > 0.0) || tile.nodes.empty())
        return;
    kind_ = Kind::pattern;
    // A side's length on the image is its length in the tile times how far the transforms stretch the tile's own axis
    // along it, which a turn leaves alone and a skew lengthens.
    const Affine toImage = toPixels * pattern.transform;
    tileWidth_ = tilePixels(tile.width * length(applyLinear(toImage, {1.0, 0.0})));
    tileHeight_ = tilePixels(tile.height * length(applyLinear(toImage, {0.0, 1.0})));
    tile_ = draw(tile, tileWidth_, tileHeight_);
    toTile_ = scale(tileWidth_ / tile.width, tileHeight_ / tile.height) * inverse(pattern.transform);
}

LinearColor Shader::at(Vec2 p) const {
    switch (kind_) {
    case Kind::solid:
        return solid_;
    case Kind::linear:
        return along(spreadOffset(axisLength2_ > 0.0 ? dot(p - from_, axis_) / axisLength2_ : 0.0, spread_));
    case Kind::radial:
        return along(spreadOffset(length(p - from_) / radius_, spread_));
    case Kind::twoCircle: {
        const std::optional<double> t = twoCircleOffset(p);
        return t ? along(spreadOffset(*t, spread_)) : LinearColor{};
    }
    case Kind::conic: {
        const Vec2 d = p - from_;
        const double turns = (std::atan2(d.y, d.x) - startAngle_) / fullTurn;
        return along(turns - std::floor(turns));
    }
    case Kind::pattern:
        return tileAt(p);
    }
    return solid_;
}

std::optional<double> Shader::twoCircleOffset(Vec2 p) const {
    // With q = p - c0, d = c1 - c0 and g = r1 - r0, p lies on the circle of offset t when |q - t d| = r0 + t g, which
    // squares to a t^2 - 2 b t + c = 0 with a = |d|^2 - g^2, b = q . d + r0 g and c = |q|^2 - r0^2, a root of the
    // squared equation being a root of the first wherever the radius r0 + t g is not negative. Of those roots, p takes
    // the greatest.
    const Vec2 q = p - from_;
    const double b = dot(q, axis_) + startRadius_ * growth_;
    const double c = dot(q, q) - startRadius_ * startRadius_;
    const auto drawn = [&](double t) { return startRadius_ + t * growth_ >= 0.0; };
    if (leading_ == 0.0) {
        // One circle touches the other from inside, and the equation is linear: one root, or none where b is 0, as b
        // is everywhere when the two circles are one.
        if (b == 0.0)
            return std::nullopt;
        const double t = c / (2.0 * b);
        return drawn(t) ? std::optional(t) : std::nullopt;
    }
    const double discriminant = b * b - leading_ * c;
    if (discriminant < 0.0)
        return std::nullopt;
    // The roots are k / a and c / k for k = b + sign(b) sqrt(discriminant), a form that never subtracts nearly equal
    // numbers. k is 0 only when b and c both are, for a double root at 0.
    const double k = b + std::copysign(std::sqrt(discriminant), b);
    const double first = k / leading_;
    const double second = k != 0.0 ? c / k : first;
    const double greater = std::max(first, second);
    const double lesser = std::min(first, second);
    if (drawn(greater))
        return greater;
    if (drawn(lesser))
        return lesser;
    return std::nullopt;
}

LinearColor Shader::along(double t) const {
    if (stops_.empty())
        return {};
    Stop mixed = stops_.front();
    if (t >= stops_.back().offset) {
        mixed = stops_.back();
    } else if (t > stops_.front().offset) {
        const auto after = std::upper_bound(stops_.begin(), stops_.end(), t,
                                            [](double offset, const Stop &stop) { return offset < stop.offset; });
        const Stop &left = *(after - 1);
        const Stop &right = *after;
        const double span = right.offset - left.offset;
        const double u = span > 0.0 ? (t - left.offset) / span : 1.0;
        mixed = {t, left.x + (right.x - left.x) * u, left.y + (right.y - left.y) * u, left.z + (right.z - left.z) * u,
                 left.alpha + (right.alpha - left.alpha) * u};
    }
    if (mixed.alpha <= 0.0)
        return {};
    const double x = mixed.x / mixed.alpha;
    const double y = mixed.y / mixed.alpha;
    const double z = mixed.z / mixed.alpha;
    LinearColor c{};
    switch (interpolation_) {
    case Interpolation::oklab:
        c = oklabToLinear({x, y, z, mixed.alpha});
        break;
    case Interpolation::srgb:
        c = {decodeSrgb(x), decodeSrgb(y), decodeSrgb(z), mixed.alpha};
        break;
    case Interpolation::srgbLinear:
        c = {x, y, z, mixed.alpha};
        break;
    }
    // A blend in Oklab of colors inside the sRGB gamut can stray just outside it, and a blend in sRGB by rounding, so
    // each channel clips back to it.
    return {std::clamp(c.r, 0.0, 1.0), std::clamp(c.g, 0.0, 1.0), std::clamp(c.b, 0.0, 1.0), c.a};
}

LinearColor Shader::tileAt(Vec2 p) const {
    // Bilinear interpolation between the four tile pixels whose centers surround p, each wrapped around the tile.
    const Vec2 q = apply(toTile_, p) - Vec2{0.5, 0.5};
    if (!std::isfinite(q.x) || !std::isfinite(q.y))
        return {};
    const double left = std::floor(q.x);
    const double top = std::floor(q.y);
    const double u = q.x - left;
    const double v = q.y - top;
    const auto width = static_cast<std::size_t>(tileWidth_);
    const auto height = static_cast<std::size_t>(tileHeight_);
    const std::size_t x0 = wrapped(left, tileWidth_);
    const std::size_t x1 = x0 + 1 == width ? 0 : x0 + 1;
    const std::size_t y0 = wrapped(top, tileHeight_);
    const std::size_t y1 = y0 + 1 == height ? 0 : y0 + 1;
    Pixel sum{};
    const auto add = [&](std::size_t x, std::size_t y, double weight) {
        const Pixel &texel = tile_[y * width + x];
        sum = {sum.r + texel.r * weight, sum.g + texel.g * weight, sum.b + texel.b * weight, sum.a + texel.a * weight};
    };
    add(x0, y0, (1.0 - u) * (1.0 - v));
    add(x1, y0, u * (1.0 - v));
    add(x0, y1, (1.0 - u) * v);
    add(x1, y1, u * v);
    if (sum.a <= 0.0)
        return {};
    return {sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, sum.a};
}

} // namespace linea::detail
