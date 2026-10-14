// linea - Drawing implementation: rasterize each layer's fill and stroke, then composite them over the image in
// linear light with premultiplied alpha. A translucent group, a translucent layer with both a fill and a stroke, and a
// node with effects draw on a surface of their own first, where the effects apply, then composite that surface over
// the image as one.
//
// Every step works only on the pixels it can change. A shape rasterizes and composites over the box of pixels around
// its outline. A node's own surface holds only the part of its plane that its content, spread by its blur and its
// shadow, reaches, and of that only what its effects read to give the pixels it changes beneath: those the surface
// beneath holds, within its clip. Each pixel sums the same terms in the same order as on surfaces of the whole image,
// so the image is the same.
#include "linea/Drawing.hpp"

#include "ColorSpace.hpp"
#include "Coverage.hpp"
#include "Shading.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>

namespace linea {
namespace {

constexpr double byteMax = 255.0;
// A Gaussian kernel reaches this many standard deviations each way, where it has fallen to 1.1% of its peak and holds
// 99.7% of its weight.
constexpr double kernelDeviations = 3.0;
// How many passes of a box stand in for a Gaussian: three, as SVG's feGaussianBlur takes and browsers draw it.
constexpr int boxPasses = 3;
// CSS Masking's luminance coefficients for linear RGB, those of feColorMatrix's luminanceToAlpha.
constexpr double redLuminance = 0.2125;
constexpr double greenLuminance = 0.7154;
constexpr double blueLuminance = 0.0721;

using detail::Pixel;
using detail::PixelBox;

// Part of an image being drawn: the pixels of a box within a plane, row-major from the box's top left. The plane is
// the image the drawing maps onto, which sets where edges clamp; drawing changes only the pixels the box holds.
struct Surface {
    // The plane's size.
    int width{};
    int height{};
    PixelBox box;
    std::vector<Pixel> pixels;

    // Pixel (@p x, @p y) of the plane, which the box must hold.
    Pixel &at(int x, int y) { return pixels[box.index(x, y)]; }
};

// A transparent surface for the pixels of @p box, which must hold some, in a plane @p width by @p height.
Surface blank(int width, int height, PixelBox box) {
    return {width, height, box,
            std::vector<Pixel>(static_cast<std::size_t>(box.width()) * static_cast<std::size_t>(box.height()))};
}

// @p box moved @p dx pixels right and @p dy down.
PixelBox translated(PixelBox box, int dx, int dy) { return {box.x0 + dx, box.y0 + dy, box.x1 + dx, box.y1 + dy}; }

// Composites @p source over @p dest, both premultiplied.
void over(Pixel &dest, const Pixel &source) {
    const double keep = 1.0 - source.a;
    dest = {source.r + dest.r * keep, source.g + dest.g * keep, source.b + dest.b * keep, source.a + dest.a * keep};
}

// Composites @p shader over @p surface wherever @p cover covers it, sampling the paint at each pixel's center mapped
// back through @p toPaint.
void composite(Surface &surface, const detail::BoxCoverage &cover, const detail::Shader &shader, const Affine &toPaint,
               double opacity) {
    const PixelBox box = intersection(cover.box, surface.box);
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            const float c = cover.values[cover.box.index(x, y)];
            if (c <= 0.0f)
                continue;
            const detail::LinearColor color =
                shader.at(apply(toPaint, {static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5}));
            const double alpha = color.a * static_cast<double>(c) * opacity;
            if (alpha <= 0.0)
                continue;
            over(surface.at(x, y), {color.r * alpha, color.g * alpha, color.b * alpha, alpha});
        }
    }
}

std::vector<Contour> mapped(std::vector<Contour> contours, const Affine &m) {
    for (Contour &contour : contours)
        for (Vec2 &p : contour.points)
            p = apply(m, p);
    return contours;
}

// A layer's fill and stroke as outlines in pixels, each empty for a paint the layer lacks.
struct Outlines {
    std::vector<Contour> fill;
    std::vector<Contour> stroke;
};

// The outlines of @p layer, whose path @p toPixels maps to pixels.
Outlines outlines(const Layer &layer, const Affine &toPixels) {
    Outlines drawn;
    if (layer.fill)
        drawn.fill = flatten(transformed(layer.path, toPixels), renderTolerance);
    const double stretch = maxScale(toPixels);
    if (layer.stroke && stretch > 0.0) {
        // The stroke is built in the path's own coordinates, so a transform widens it as it widens the path, at a
        // tolerance that stays renderTolerance once the transform scales it.
        const double tolerance = renderTolerance / stretch;
        drawn.stroke = mapped(strokeOutline(flatten(layer.path, tolerance), layer.strokeStyle, tolerance), toPixels);
    }
    return drawn;
}

// The pixels of a plane @p width by @p height that @p drawn can cover.
PixelBox pixelBounds(const Outlines &drawn, int width, int height) {
    return hull(detail::pixelBounds(drawn.fill, width, height), detail::pixelBounds(drawn.stroke, width, height));
}

// @p clip's path in pixels, @p toPixels mapping the clipped node's coordinates to them.
std::vector<Contour> clipOutline(const Clip &clip, const Affine &toPixels) {
    return flatten(transformed(clip.path, toPixels), renderTolerance);
}

void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent);

// Weights that convolve a plane along one axis: the result at x is the sum of weights[k] times the value at
// x + first + k.
struct Kernel {
    int first{};
    std::vector<double> weights;
};

// The kernel that changes nothing.
Kernel unchanged() { return {0, {1.0}}; }

// How many pixels before the one it writes @p kernel reads, and how many after.
int reachBefore(const Kernel &kernel) { return std::max(0, -kernel.first); }
int reachAfter(const Kernel &kernel) { return std::max(0, kernel.first + static_cast<int>(kernel.weights.size()) - 1); }

// An extended box (Gwosdek, Grewenig, Bruhn and Weickert, 2011): the mean of the 2 radius + 1 pixels centered on the
// one it writes and of a fraction of the pixel beyond each end, as if the box reached that much of a pixel farther. The
// fraction lets the box's variance take any value, where whole widths give it only in steps.
struct Box {
    int radius{};
    double fraction{};
};

// How a blur or a shadow spreads a plane along one axis: through boxPasses passes of a box, for a Gaussian whose
// deviation calls for them, then through a kernel, for a Gaussian sampled at whole pixels or a shadow's offset. Either
// may be missing, and a spread with neither changes nothing.
struct Spread {
    std::optional<Box> box;
    std::optional<Kernel> kernel;
};

// How many pixels before the one it writes @p spread reads, and how many after: each pass of the box reads its radius
// and the pixel beyond.
int reachBefore(const Spread &spread) {
    return (spread.box ? boxPasses * (spread.box->radius + 1) : 0) + (spread.kernel ? reachBefore(*spread.kernel) : 0);
}
int reachAfter(const Spread &spread) {
    return (spread.box ? boxPasses * (spread.box->radius + 1) : 0) + (spread.kernel ? reachAfter(*spread.kernel) : 0);
}

// The box whose boxPasses passes spread as a Gaussian of deviation @p sigma pixels does: variances add, so each pass
// takes a share of sigma^2. A box of radius r and fraction a has variance (r (r + 1) (2 r + 1) / 3 + 2 a (r + 1)^2) /
// (2 r + 1 + 2 a): r (r + 1) / 3 with a = 0, and (r + 1) (r + 2) / 3 with a = 1, which is the box of radius r + 1. So r
// is the largest radius whose variance with a = 0 is within the share, and a gives the rest. The radius is at most what
// keeps every pass within effectReach, with a at most 1 there.
Box box(double sigma) {
    const double share = sigma * sigma / boxPasses;
    const double tripled = 3.0 * share;
    int radius = static_cast<int>(std::floor((std::sqrt(1.0 + 4.0 * tripled) - 1.0) / 2.0));
    // The square root rounds, which can leave the radius one off where r (r + 1) meets three times the share.
    if ((radius + 1.0) * (radius + 2.0) <= tripled)
        ++radius;
    if (radius * (radius + 1.0) > tripled)
        --radius;
    constexpr int widest = effectReach / boxPasses - 1;
    if (radius > widest)
        return {widest, 1.0};
    const double r = radius;
    return {radius, (2.0 * r + 1.0) * (tripled - r * (r + 1.0)) / (6.0 * ((r + 1.0) * (r + 1.0) - share))};
}

// A Gaussian of standard deviation @p deviation pixels, at most a third of effectReach: boxPasses passes of a box from
// boxBlurDeviation up, and below that the Gaussian sampled at whole pixels out to kernelDeviations each way and scaled
// to sum to 1. Either keeps total alpha. A deviation that is not positive gives the spread that changes nothing.
Spread gaussian(double deviation) {
    if (!(deviation > 0.0))
        return {};
    const double sigma = std::min(deviation, static_cast<double>(effectReach) / kernelDeviations);
    if (sigma >= boxBlurDeviation)
        return {box(sigma), std::nullopt};
    const int radius = static_cast<int>(std::ceil(kernelDeviations * sigma));
    Kernel kernel{-radius, {}};
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double z = i / sigma;
        kernel.weights.push_back(std::exp(-0.5 * z * z));
        sum += kernel.weights.back();
    }
    for (double &weight : kernel.weights)
        weight /= sum;
    return {std::nullopt, kernel};
}

// @p kernel moved @p shift pixels forward along its axis: its weights move by the whole pixels of @p shift, and each
// splits between the two pixels the fraction left falls between, as linear interpolation does. The kernel's sum stays,
// and its centroid moves exactly @p shift. @p shift must fit an int.
Kernel shifted(const Kernel &kernel, double shift) {
    const double whole = std::floor(shift);
    const double fraction = shift - whole;
    Kernel moved{kernel.first - static_cast<int>(whole) - 1, std::vector<double>(kernel.weights.size() + 1)};
    for (std::size_t k = 0; k < kernel.weights.size(); ++k) {
        moved.weights[k] += fraction * kernel.weights[k];
        moved.weights[k + 1] += (1.0 - fraction) * kernel.weights[k];
    }
    return moved;
}

// @p spread moved @p shift pixels forward: its kernel, or one that changes nothing, moved.
Spread shifted(Spread spread, double shift) {
    spread.kernel = shifted(spread.kernel.value_or(unchanged()), shift);
    return spread;
}

void addScaled(Pixel &into, const Pixel &from, double weight) {
    into.r += from.r * weight;
    into.g += from.g * weight;
    into.b += from.b * weight;
    into.a += from.a * weight;
}

void addScaled(double &into, double from, double weight) { into += from * weight; }

// @p in, rows of @p columns values, convolved along x with @p kernel into @p out. Only the taps that read within the
// row count.
template <class Value>
void kernelAlongX(const std::vector<Value> &in, std::vector<Value> &out, std::ptrdiff_t columns, const Kernel &kernel) {
    std::ranges::fill(out, Value{});
    const std::ptrdiff_t rowCount = std::ssize(in) / columns;
    for (std::ptrdiff_t y = 0; y < rowCount; ++y) {
        const Value *row = in.data() + y * columns;
        Value *written = out.data() + y * columns;
        for (std::ptrdiff_t x = 0; x < columns; ++x) {
            const std::ptrdiff_t start = x + kernel.first;
            const std::ptrdiff_t to = std::min(std::ssize(kernel.weights), columns - start);
            for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, -start); k < to; ++k)
                addScaled(written[x], row[start + k], kernel.weights[static_cast<std::size_t>(k)]);
        }
    }
}

// @p in, rows of @p columns values, convolved along y with @p kernel into @p out. Only the taps that read within the
// plane count, each adding a whole row.
template <class Value>
void kernelAlongY(const std::vector<Value> &in, std::vector<Value> &out, std::ptrdiff_t columns, const Kernel &kernel) {
    std::ranges::fill(out, Value{});
    const std::ptrdiff_t rowCount = std::ssize(in) / columns;
    for (std::ptrdiff_t y = 0; y < rowCount; ++y) {
        Value *written = out.data() + y * columns;
        const std::ptrdiff_t start = y + kernel.first;
        const std::ptrdiff_t to = std::min(std::ssize(kernel.weights), rowCount - start);
        for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, -start); k < to; ++k) {
            const Value *row = in.data() + (start + k) * columns;
            const double weight = kernel.weights[static_cast<std::size_t>(k)];
            for (std::ptrdiff_t x = 0; x < columns; ++x)
                addScaled(written[x], row[x], weight);
        }
    }
}

// The factor that turns the sum a box weighs into its mean.
double boxScale(const Box &box) { return 1.0 / (2.0 * box.radius + 1.0 + 2.0 * box.fraction); }

// One value a box writes: @p scale times the sum of the whole pixels it covers, @p upTo less @p from, two prefix sums,
// and of its fraction of @p before and @p after, the values beyond each end, each null past the end of the line. Values
// are never negative, so a prefix sum never falls as it adds them, which makes the result at least 0, and exactly 0
// where every value the box covers is 0; a running sum would drift by rounding instead.
template <class Value>
Value boxValue(const Value &upTo, const Value &from, const Value *before, const Value *after, const Box &box,
               double scale) {
    Value sum = upTo;
    addScaled(sum, from, -1.0);
    if (before)
        addScaled(sum, *before, box.fraction);
    if (after)
        addScaled(sum, *after, box.fraction);
    Value mean{};
    addScaled(mean, sum, scale);
    return mean;
}

// @p in, rows of @p columns values, boxed along x into @p out.
template <class Value>
void boxAlongX(const std::vector<Value> &in, std::vector<Value> &out, std::ptrdiff_t columns, const Box &box) {
    const double scale = boxScale(box);
    // prefix[x] holds the sum of the row's first x values.
    std::vector<Value> prefix(static_cast<std::size_t>(columns) + 1);
    for (std::ptrdiff_t y = 0; y < std::ssize(in) / columns; ++y) {
        const Value *row = in.data() + y * columns;
        for (std::ptrdiff_t x = 0; x < columns; ++x) {
            prefix[static_cast<std::size_t>(x + 1)] = prefix[static_cast<std::size_t>(x)];
            addScaled(prefix[static_cast<std::size_t>(x + 1)], row[x], 1.0);
        }
        for (std::ptrdiff_t x = 0; x < columns; ++x) {
            const std::ptrdiff_t before = x - box.radius - 1;
            const std::ptrdiff_t after = x + box.radius + 1;
            out[static_cast<std::size_t>(y * columns + x)] =
                boxValue(prefix[static_cast<std::size_t>(std::min(columns, after))],
                         prefix[static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, before + 1))],
                         before >= 0 ? row + before : nullptr, after < columns ? row + after : nullptr, box, scale);
        }
    }
}

// @p in, rows of @p columns values, boxed along y into @p out, a row at a time so that memory is read in order.
template <class Value>
void boxAlongY(const std::vector<Value> &in, std::vector<Value> &out, std::ptrdiff_t columns, const Box &box) {
    const double scale = boxScale(box);
    const std::ptrdiff_t rowCount = std::ssize(in) / columns;
    // Row y of prefix holds the sums of each column's first y values.
    std::vector<Value> prefix(static_cast<std::size_t>((rowCount + 1) * columns));
    for (std::ptrdiff_t i = 0; i < std::ssize(in); ++i) {
        prefix[static_cast<std::size_t>(i + columns)] = prefix[static_cast<std::size_t>(i)];
        addScaled(prefix[static_cast<std::size_t>(i + columns)], in[static_cast<std::size_t>(i)], 1.0);
    }
    for (std::ptrdiff_t y = 0; y < rowCount; ++y) {
        const std::ptrdiff_t before = y - box.radius - 1;
        const std::ptrdiff_t after = y + box.radius + 1;
        const Value *upTo = prefix.data() + std::min(rowCount, after) * columns;
        const Value *from = prefix.data() + std::max<std::ptrdiff_t>(0, before + 1) * columns;
        const Value *above = before >= 0 ? in.data() + before * columns : nullptr;
        const Value *below = after < rowCount ? in.data() + after * columns : nullptr;
        for (std::ptrdiff_t x = 0; x < columns; ++x)
            out[static_cast<std::size_t>(y * columns + x)] =
                boxValue(upTo[x], from[x], above ? above + x : nullptr, below ? below + x : nullptr, box, scale);
    }
}

// @p plane, rows of @p width values, spread along x by @p alongX and then along y by @p alongY. What would land beyond
// the plane is lost.
template <class Value> void convolve(std::vector<Value> &plane, int width, const Spread &alongX, const Spread &alongY) {
    const auto columns = static_cast<std::ptrdiff_t>(width);
    std::vector<Value> other(plane.size());
    // Each step reads the plane and writes the other buffer, which then holds the plane.
    const auto step = [&](const auto &apply) {
        apply(std::as_const(plane), other);
        std::swap(plane, other);
    };
    for (int pass = 0; alongX.box && pass < boxPasses; ++pass)
        step([&](const std::vector<Value> &in, std::vector<Value> &out) { boxAlongX(in, out, columns, *alongX.box); });
    if (alongX.kernel)
        step([&](const std::vector<Value> &in, std::vector<Value> &out) {
            kernelAlongX(in, out, columns, *alongX.kernel);
        });
    for (int pass = 0; alongY.box && pass < boxPasses; ++pass)
        step([&](const std::vector<Value> &in, std::vector<Value> &out) { boxAlongY(in, out, columns, *alongY.box); });
    if (alongY.kernel)
        step([&](const std::vector<Value> &in, std::vector<Value> &out) {
            kernelAlongY(in, out, columns, *alongY.kernel);
        });
}

// Composites beneath what @p surface holds its own alpha, spread by @p alongX and @p alongY and filled with @p color:
// a drop shadow.
void castShadow(Surface &surface, const Spread &alongX, const Spread &alongY, Color color) {
    std::vector<double> shade(surface.pixels.size());
    std::ranges::transform(surface.pixels, shade.begin(), &Pixel::a);
    convolve(shade, surface.box.width(), alongX, alongY);
    const detail::LinearColor fill = detail::linear(color);
    for (std::size_t i = 0; i < shade.size(); ++i) {
        const double a = shade[i] * fill.a;
        if (!(a > 0.0))
            continue;
        Pixel beneath{fill.r * a, fill.g * a, fill.b * a, a};
        over(beneath, surface.pixels[i]);
        surface.pixels[i] = beneath;
    }
}

// The factor by which @p mask scales the alpha of the group it masks, at each pixel of @p box, which must hold some,
// in a plane @p width by @p height. @p toPixels maps the group's coordinates to the plane's pixels.
std::vector<double> maskFactors(const Mask &mask, int width, int height, PixelBox box, const Affine &toPixels) {
    Surface drawn = blank(width, height, box);
    drawNodes(drawn, mask.nodes, toPixels);
    std::vector<double> factors(drawn.pixels.size());
    std::ranges::transform(drawn.pixels, factors.begin(), [&](const Pixel &p) {
        // Premultiplied, the luminance of the channels is already the luminance times the alpha.
        return mask.mode == MaskMode::alpha ? p.a : redLuminance * p.r + greenLuminance * p.g + blueLuminance * p.b;
    });
    return factors;
}

// What a node does to its own image between drawing it and compositing it, in the order SVG applies them: the blur,
// the shadow, the clip, the mask and the opacity. Each pointer is null for an effect the node lacks.
struct Effects {
    double blur{};
    const DropShadow *shadow{};
    const Clip *clip{};
    const Mask *mask{};
    double opacity = 1.0;

    // Whether the node has any effect beyond its opacity.
    [[nodiscard]] bool any() const { return blur > 0.0 || shadow || clip || mask; }
};

// What @p optional holds, or null.
template <class T> const T *held(const std::optional<T> &optional) { return optional ? &*optional : nullptr; }

// The effects of a layer or a group, with its opacity clamped to 0 to 1.
Effects effectsOf(const Layer &layer) {
    return {layer.blur, held(layer.shadow), held(layer.clip), nullptr, std::clamp(layer.opacity, 0.0, 1.0)};
}
Effects effectsOf(const Group &group) {
    return {group.blur, held(group.shadow), held(group.clip), held(group.mask), std::clamp(group.opacity, 0.0, 1.0)};
}

// Whether a layer or a group with @p effects draws anything: one that fades out entirely, or has nothing to draw,
// changes no pixel.
bool shows(const Layer &layer, const Effects &effects) { return effects.opacity > 0.0 && !layer.path.empty(); }
bool shows(const Group &group, const Effects &effects) { return effects.opacity > 0.0 && !group.children.empty(); }

// Whether @p layer composites its fill and stroke straight onto the surface beneath. At full opacity nothing fades. A
// fill or a stroke alone is one coverage composited once, so scaling its alpha by the opacity gives exactly what fading
// it on a surface of its own would, without that surface.
bool direct(const Layer &layer, const Effects &effects) {
    return !effects.any() && (effects.opacity >= 1.0 || !layer.fill || !layer.stroke);
}

// Whether a group draws its children straight onto the surface beneath. Compositing over is associative, so drawing
// the children one by one onto the surface gives what drawing them on a surface of their own and compositing that
// would, without the second surface.
bool direct(const Group & /*group*/, const Effects &effects) { return !effects.any() && effects.opacity >= 1.0; }

// How a node with effects draws on a surface of its own before compositing onto the plane beneath: the kernels of its
// blur and its shadow, and its own plane. That plane is the one beneath widened on each side as far as the kernels
// read, at most effectReach, so that what lies just outside can blur or cast a shadow into view.
struct Isolation {
    Spread blurX;
    Spread blurY;
    // The shadow, or null for none or for one that falls too far to show.
    const DropShadow *shadow{};
    Spread shadowX;
    Spread shadowY;
    // How far the node's plane reaches left of the one beneath and above it, and its size.
    int left{};
    int top{};
    int width{};
    int height{};
    // Maps the node's coordinates to its plane's pixels.
    Affine toPixels;
};

// How a node with @p effects draws, @p toPixels mapping its coordinates to a plane @p width by @p height.
Isolation isolation(const Effects &effects, const Affine &toPixels, int width, int height) {
    // A Gaussian of deviation s in the node's coordinates, mapped to pixels by M, has covariance s^2 M M^T, whose
    // diagonal gives its deviation along x and y: s times the length of each row of M.
    const double stretchX = std::hypot(toPixels.a, toPixels.c);
    const double stretchY = std::hypot(toPixels.b, toPixels.d);
    Isolation plan;
    plan.blurX = gaussian(effects.blur * stretchX);
    plan.blurY = gaussian(effects.blur * stretchY);

    const DropShadow *shadow = effects.shadow;
    const Vec2 offset = shadow ? applyLinear(toPixels, shadow->offset) : Vec2{};
    // Content on the node's plane lies within effectReach of the plane beneath, and its shadow spreads at most
    // effectReach more, so a shadow that falls farther than both and that plane itself never shows. Dropping it keeps
    // the offset within an int, and drops one that is not finite.
    if (shadow && !(std::abs(offset.x) < width + 2.0 * effectReach && std::abs(offset.y) < height + 2.0 * effectReach))
        shadow = nullptr;
    plan.shadow = shadow;
    if (shadow) {
        plan.shadowX = shifted(gaussian(shadow->deviation * stretchX), offset.x);
        plan.shadowY = shifted(gaussian(shadow->deviation * stretchY), offset.y);
    }

    // The shadow reads the blurred image as far as its kernel reaches, and the blur reads the drawn one as far again.
    plan.left = std::min(effectReach, reachBefore(plan.blurX) + reachBefore(plan.shadowX));
    const int right = std::min(effectReach, reachAfter(plan.blurX) + reachAfter(plan.shadowX));
    plan.top = std::min(effectReach, reachBefore(plan.blurY) + reachBefore(plan.shadowY));
    const int bottom = std::min(effectReach, reachAfter(plan.blurY) + reachAfter(plan.shadowY));
    plan.width = width + plan.left + right;
    plan.height = height + plan.top + bottom;
    plan.toPixels = translate({static_cast<double>(plan.left), static_cast<double>(plan.top)}) * toPixels;
    return plan;
}

// The pixels of a node's plane that its blur and its shadow carry what lies in @p box to. A kernel that reads a pixel
// some way before the one it writes carries each pixel that far after it, and the other way about.
PixelBox spread(PixelBox box, const Isolation &plan) {
    return {box.x0 - reachAfter(plan.blurX) - reachAfter(plan.shadowX),
            box.y0 - reachAfter(plan.blurY) - reachAfter(plan.shadowY),
            box.x1 + reachBefore(plan.blurX) + reachBefore(plan.shadowX),
            box.y1 + reachBefore(plan.blurY) + reachBefore(plan.shadowY)};
}

// The pixels of a node's plane that its blur and its shadow read to give those of @p box.
PixelBox sources(PixelBox box, const Isolation &plan) {
    return {box.x0 - reachBefore(plan.blurX) - reachBefore(plan.shadowX),
            box.y0 - reachBefore(plan.blurY) - reachBefore(plan.shadowY),
            box.x1 + reachAfter(plan.blurX) + reachAfter(plan.shadowX),
            box.y1 + reachAfter(plan.blurY) + reachAfter(plan.shadowY)};
}

// The pixels of a plane @p width by @p height that a node drawn as @p plan changes, before its clip, when its content
// covers @p content of its own plane. Content that covers nothing changes nothing, however far the kernels reach.
PixelBox changed(const Isolation &plan, PixelBox content, int width, int height) {
    if (content.empty())
        return {};
    return intersection(translated(spread(content, plan), -plan.left, -plan.top), {0, 0, width, height});
}

PixelBox reach(const std::vector<Node> &nodes, const Affine &parent, int width, int height);

// The pixels of a plane @p width by @p height that drawing @p layer or @p group changes, @p parent mapping the
// coordinates it sits in to the plane's pixels. Drawing changes no pixel outside them.
PixelBox reach(const Layer &layer, const Affine &parent, int width, int height) {
    const Effects effects = effectsOf(layer);
    if (!shows(layer, effects))
        return {};
    const Affine toPixels = parent * layer.transform;
    if (direct(layer, effects))
        return pixelBounds(outlines(layer, toPixels), width, height);
    const Isolation plan = isolation(effects, toPixels, width, height);
    const PixelBox box =
        changed(plan, pixelBounds(outlines(layer, plan.toPixels), plan.width, plan.height), width, height);
    return effects.clip ? intersection(box, detail::pixelBounds(clipOutline(*effects.clip, toPixels), width, height))
                        : box;
}

PixelBox reach(const Group &group, const Affine &parent, int width, int height) {
    const Effects effects = effectsOf(group);
    if (!shows(group, effects))
        return {};
    const Affine toPixels = parent * group.transform;
    if (direct(group, effects))
        return reach(group.children, toPixels, width, height);
    const Isolation plan = isolation(effects, toPixels, width, height);
    const PixelBox box = changed(plan, reach(group.children, plan.toPixels, plan.width, plan.height), width, height);
    return effects.clip ? intersection(box, detail::pixelBounds(clipOutline(*effects.clip, toPixels), width, height))
                        : box;
}

// The pixels that drawing the group @p group holds changes.
PixelBox reach(const Indirect<Group> &group, const Affine &parent, int width, int height) {
    return reach(*group, parent, width, height);
}

PixelBox reach(const std::vector<Node> &nodes, const Affine &parent, int width, int height) {
    PixelBox box;
    for (const Node &node : nodes) {
        const PixelBox drawn =
            std::visit([&](const auto &content) { return reach(content, parent, width, height); }, node.content);
        box = hull(box, drawn);
    }
    return box;
}

// Calls @p drawOn with a transparent surface on @p plan's plane, applies @p effects to what it drew, and composites
// the result over @p surface as one image. @p toPixels maps the node's coordinates to @p surface's pixels, and what the
// node draws covers @p content of its own plane.
//
// Only the pixels of @p surface that the node changes composite: those its content reaches once the blur and the
// shadow spread it, that @p surface holds, within the clip. The node's surface holds only the pixels of its plane that
// its effects read to give those, and that its content reaches; every pixel the effects read beyond them is one the
// content leaves transparent, so each of those pixels takes the same terms, in the same order, as over the whole
// plane.
void drawIsolated(Surface &surface, const Affine &toPixels, const Isolation &plan, const Effects &effects,
                  PixelBox content, const std::invocable<Surface &> auto &drawOn) {
    PixelBox box = intersection(changed(plan, content, surface.width, surface.height), surface.box);
    detail::BoxCoverage clipCover;
    if (effects.clip) {
        clipCover = detail::boxCoverage(clipOutline(*effects.clip, toPixels), surface.width, surface.height,
                                        effects.clip->fillRule);
        box = intersection(box, clipCover.box);
    }
    if (box.empty())
        return;

    const PixelBox plane{0, 0, plan.width, plan.height};
    const PixelBox needed = sources(translated(box, plan.left, plan.top), plan);
    Surface isolated = blank(plan.width, plan.height, intersection(intersection(spread(content, plan), needed), plane));
    drawOn(isolated);

    if (effects.blur > 0.0)
        convolve(isolated.pixels, isolated.box.width(), plan.blurX, plan.blurY);
    if (plan.shadow)
        castShadow(isolated, plan.shadowX, plan.shadowY, plan.shadow->color);
    std::vector<double> maskFactor;
    if (effects.mask)
        maskFactor = maskFactors(*effects.mask, surface.width, surface.height, box, toPixels);

    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            const Pixel &p = isolated.at(x + plan.left, y + plan.top);
            double factor = effects.opacity;
            if (effects.clip)
                factor *= static_cast<double>(clipCover.values[clipCover.box.index(x, y)]);
            if (effects.mask)
                factor *= maskFactor[box.index(x, y)];
            if (p.a > 0.0 && factor > 0.0)
                over(surface.at(x, y), {p.r * factor, p.g * factor, p.b * factor, p.a * factor});
        }
    }
}

// Composites @p drawn, the outlines of @p layer, fill and then stroke, over @p surface, each with its alpha scaled by
// @p opacity. @p toPixels maps the path's coordinates to pixels.
void paint(Surface &surface, const Layer &layer, const Outlines &drawn, const Affine &toPixels, double opacity) {
    const Affine toPaint = inverse(toPixels);
    const auto paintOver = [&](const std::vector<Contour> &contours, const Paint &with, FillRule rule) {
        const detail::BoxCoverage cover = detail::boxCoverage(contours, surface.width, surface.height, rule);
        // A paint is made ready to sample, which draws a pattern's tile, only where it shows.
        if (!intersection(cover.box, surface.box).empty())
            composite(surface, cover, detail::Shader(with, toPixels), toPaint, opacity);
    };
    if (layer.fill)
        paintOver(drawn.fill, *layer.fill, layer.fillRule);
    if (layer.stroke)
        paintOver(drawn.stroke, *layer.stroke, FillRule::nonZero);
}

// Draws @p layer on @p surface. @p parent maps the coordinates the layer sits in to pixels.
void draw(Surface &surface, const Layer &layer, const Affine &parent) {
    const Effects effects = effectsOf(layer);
    if (!shows(layer, effects))
        return;
    const Affine toPixels = parent * layer.transform;
    if (direct(layer, effects)) {
        paint(surface, layer, outlines(layer, toPixels), toPixels, effects.opacity);
        return;
    }
    const Isolation plan = isolation(effects, toPixels, surface.width, surface.height);
    const Outlines drawn = outlines(layer, plan.toPixels);
    drawIsolated(surface, toPixels, plan, effects, pixelBounds(drawn, plan.width, plan.height),
                 [&](Surface &isolated) { paint(isolated, layer, drawn, plan.toPixels, 1.0); });
}

// Draws @p group on @p surface. @p parent maps the coordinates the group sits in to pixels.
void draw(Surface &surface, const Group &group, const Affine &parent) {
    const Effects effects = effectsOf(group);
    if (!shows(group, effects))
        return;
    const Affine toPixels = parent * group.transform;
    if (direct(group, effects)) {
        drawNodes(surface, group.children, toPixels);
        return;
    }
    const Isolation plan = isolation(effects, toPixels, surface.width, surface.height);
    drawIsolated(surface, toPixels, plan, effects, reach(group.children, plan.toPixels, plan.width, plan.height),
                 [&](Surface &isolated) { drawNodes(isolated, group.children, plan.toPixels); });
}

// Draws the group that @p group holds on @p surface.
void draw(Surface &surface, const Indirect<Group> &group, const Affine &parent) { draw(surface, *group, parent); }

// Draws @p nodes on @p surface in order. @p parent maps the coordinates the nodes sit in to pixels.
void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent) {
    for (const Node &node : nodes)
        std::visit([&](const auto &content) { draw(surface, content, parent); }, node.content);
}

} // namespace

namespace detail {

std::vector<Pixel> draw(const Drawing &drawing, int width, int height) {
    Surface surface = blank(width, height, {0, 0, width, height});
    drawNodes(surface, drawing.nodes, scale(width / drawing.width, height / drawing.height));
    return std::move(surface.pixels);
}

} // namespace detail

Image render(const Drawing &drawing, int pixelWidth, int pixelHeight) {
    const int width = std::max(1, pixelWidth);
    const int height = std::max(1, pixelHeight);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    Image image{width, height, std::vector<std::uint8_t>(count * 4, 0)};
    if (!(drawing.width > 0.0) || !(drawing.height > 0.0))
        return image;

    const std::vector<Pixel> pixels = detail::draw(drawing, width, height);
    for (std::size_t i = 0; i < count; ++i) {
        const Pixel &p = pixels[i];
        if (p.a <= 0.0)
            continue;
        std::uint8_t *out = image.rgba.data() + i * 4;
        out[0] = linearToSrgb(p.r / p.a);
        out[1] = linearToSrgb(p.g / p.a);
        out[2] = linearToSrgb(p.b / p.a);
        out[3] = static_cast<std::uint8_t>(std::lround(std::clamp(p.a, 0.0, 1.0) * byteMax));
    }
    return image;
}

} // namespace linea
