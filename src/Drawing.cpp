// linea - Drawing implementation: rasterize each layer's fill and stroke, then composite them over the image in
// linear light with premultiplied alpha. A translucent group, a translucent layer with both a fill and a stroke, and a
// node with effects draw on a surface of their own first, where the effects apply, then composite that surface over
// the image as one.
#include "linea/Drawing.hpp"

#include "ColorSpace.hpp"
#include "Shading.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <utility>

namespace linea {
namespace {

constexpr double byteMax = 255.0;
// A Gaussian kernel reaches this many standard deviations each way, where it has fallen to 1.1% of its peak and holds
// 99.7% of its weight.
constexpr double kernelDeviations = 3.0;
// CSS Masking's luminance coefficients for linear RGB, those of feColorMatrix's luminanceToAlpha.
constexpr double redLuminance = 0.2125;
constexpr double greenLuminance = 0.7154;
constexpr double blueLuminance = 0.0721;

using detail::Pixel;

// An image being drawn, row-major from the top left.
struct Surface {
    int width{};
    int height{};
    std::vector<Pixel> pixels;
};

// Composites @p source over @p dest, both premultiplied.
void over(Pixel &dest, const Pixel &source) {
    const double keep = 1.0 - source.a;
    dest = {source.r + dest.r * keep, source.g + dest.g * keep, source.b + dest.b * keep, source.a + dest.a * keep};
}

// Composites @p shader over @p surface wherever @p cover covers it, sampling the paint at each pixel's center mapped
// back through @p toPaint.
void composite(Surface &surface, const std::vector<float> &cover, const detail::Shader &shader, const Affine &toPaint,
               double opacity) {
    const auto width = static_cast<std::size_t>(surface.width);
    for (std::size_t i = 0; i < cover.size(); ++i) {
        if (cover[i] <= 0.0f)
            continue;
        const auto x = static_cast<double>(i % width);
        const auto y = static_cast<double>(i / width);
        const detail::LinearColor color = shader.at(apply(toPaint, {x + 0.5, y + 0.5}));
        const double alpha = color.a * static_cast<double>(cover[i]) * opacity;
        if (alpha <= 0.0)
            continue;
        over(surface.pixels[i], {color.r * alpha, color.g * alpha, color.b * alpha, alpha});
    }
}

std::vector<Contour> mapped(std::vector<Contour> contours, const Affine &m) {
    for (Contour &contour : contours)
        for (Vec2 &p : contour.points)
            p = apply(m, p);
    return contours;
}

void draw(Surface &surface, const Layer &layer, const Affine &parent);

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

// A Gaussian of standard deviation @p deviation pixels, at most a third of effectReach, sampled at whole pixels out to
// kernelDeviations each way and scaled to sum to 1, so that convolving with it keeps total alpha. A deviation that is
// not positive gives the kernel that changes nothing.
Kernel gaussian(double deviation) {
    if (!(deviation > 0.0))
        return unchanged();
    const double sigma = std::min(deviation, static_cast<double>(effectReach) / kernelDeviations);
    const int radius = std::min(effectReach, static_cast<int>(std::ceil(kernelDeviations * sigma)));
    Kernel kernel{-radius, {}};
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double z = i / sigma;
        kernel.weights.push_back(std::exp(-0.5 * z * z));
        sum += kernel.weights.back();
    }
    for (double &weight : kernel.weights)
        weight /= sum;
    return kernel;
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

void addScaled(Pixel &into, const Pixel &from, double weight) {
    into.r += from.r * weight;
    into.g += from.g * weight;
    into.b += from.b * weight;
    into.a += from.a * weight;
}

void addScaled(double &into, double from, double weight) { into += from * weight; }

// @p plane, rows of @p width values, convolved along x with @p alongX and then along y with @p alongY. What would land
// beyond the plane is lost.
template <class Value> void convolve(std::vector<Value> &plane, int width, const Kernel &alongX, const Kernel &alongY) {
    const auto columns = static_cast<std::ptrdiff_t>(width);
    const std::ptrdiff_t rowCount = std::ssize(plane) / columns;
    std::vector<Value> rows(plane.size());
    for (std::ptrdiff_t y = 0; y < rowCount; ++y) {
        const Value *in = plane.data() + y * columns;
        Value *out = rows.data() + y * columns;
        for (std::ptrdiff_t x = 0; x < columns; ++x) {
            // Only the taps that read within the row.
            const std::ptrdiff_t start = x + alongX.first;
            const std::ptrdiff_t to = std::min(std::ssize(alongX.weights), columns - start);
            for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, -start); k < to; ++k)
                addScaled(out[x], in[start + k], alongX.weights[static_cast<std::size_t>(k)]);
        }
    }
    std::ranges::fill(plane, Value{});
    for (std::ptrdiff_t y = 0; y < rowCount; ++y) {
        Value *out = plane.data() + y * columns;
        // Only the taps that read within the plane, each adding a whole row.
        const std::ptrdiff_t start = y + alongY.first;
        const std::ptrdiff_t to = std::min(std::ssize(alongY.weights), rowCount - start);
        for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, -start); k < to; ++k) {
            const Value *in = rows.data() + (start + k) * columns;
            const double weight = alongY.weights[static_cast<std::size_t>(k)];
            for (std::ptrdiff_t x = 0; x < columns; ++x)
                addScaled(out[x], in[x], weight);
        }
    }
}

// Composites beneath what @p surface holds its own alpha, convolved with @p alongX and @p alongY and filled with
// @p color: a drop shadow.
void castShadow(Surface &surface, const Kernel &alongX, const Kernel &alongY, Color color) {
    std::vector<double> shade(surface.pixels.size());
    std::ranges::transform(surface.pixels, shade.begin(), &Pixel::a);
    convolve(shade, surface.width, alongX, alongY);
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

// The factor by which @p mask scales the alpha of the group it masks, at each pixel of a surface @p width by @p height.
// @p toPixels maps the group's coordinates to the surface's pixels.
std::vector<double> maskFactors(const Mask &mask, int width, int height, const Affine &toPixels) {
    Surface drawn{width, height,
                  std::vector<Pixel>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height))};
    for (const Layer &layer : mask.layers)
        draw(drawn, layer, toPixels);
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

// Calls @p drawOn with a transparent surface and the transform from the node's coordinates to that surface's pixels,
// applies @p effects to what it drew, and composites the result over @p surface as one image. @p toPixels maps the
// node's coordinates to @p surface's pixels. A blur or a shadow widens the node's surface past @p surface on each side
// as far as its kernels read, at most effectReach, so that what lies just outside can blur or cast a shadow into view.
void drawIsolated(Surface &surface, const Affine &toPixels, const Effects &effects,
                  const std::invocable<Surface &, const Affine &> auto &drawOn) {
    // A Gaussian of deviation s in the node's coordinates, mapped to pixels by M, has covariance s^2 M M^T, whose
    // diagonal gives its deviation along x and y: s times the length of each row of M.
    const double stretchX = std::hypot(toPixels.a, toPixels.c);
    const double stretchY = std::hypot(toPixels.b, toPixels.d);
    const Kernel blurX = gaussian(effects.blur * stretchX);
    const Kernel blurY = gaussian(effects.blur * stretchY);

    const DropShadow *shadow = effects.shadow;
    const Vec2 offset = shadow ? applyLinear(toPixels, shadow->offset) : Vec2{};
    // Content on the node's surface lies within effectReach of @p surface, and its shadow spreads at most effectReach
    // more, so a shadow that falls farther than both and @p surface itself never shows. Dropping it keeps the offset
    // within an int, and drops one that is not finite.
    if (shadow && !(std::abs(offset.x) < surface.width + 2.0 * effectReach &&
                    std::abs(offset.y) < surface.height + 2.0 * effectReach))
        shadow = nullptr;
    const Kernel shadowX = shadow ? shifted(gaussian(shadow->deviation * stretchX), offset.x) : unchanged();
    const Kernel shadowY = shadow ? shifted(gaussian(shadow->deviation * stretchY), offset.y) : unchanged();

    // The shadow reads the blurred image as far as its kernel reaches, and the blur reads the drawn one as far again.
    const int left = std::min(effectReach, reachBefore(blurX) + reachBefore(shadowX));
    const int right = std::min(effectReach, reachAfter(blurX) + reachAfter(shadowX));
    const int top = std::min(effectReach, reachBefore(blurY) + reachBefore(shadowY));
    const int bottom = std::min(effectReach, reachAfter(blurY) + reachAfter(shadowY));
    Surface isolated{surface.width + left + right, surface.height + top + bottom, {}};
    isolated.pixels.resize(static_cast<std::size_t>(isolated.width) * static_cast<std::size_t>(isolated.height));
    drawOn(isolated, translate({static_cast<double>(left), static_cast<double>(top)}) * toPixels);

    if (effects.blur > 0.0)
        convolve(isolated.pixels, isolated.width, blurX, blurY);
    if (shadow)
        castShadow(isolated, shadowX, shadowY, shadow->color);
    std::vector<float> clipCover;
    if (effects.clip)
        clipCover = coverage(flatten(transformed(effects.clip->path, toPixels), renderTolerance), surface.width,
                             surface.height, effects.clip->fillRule);
    std::vector<double> maskFactor;
    if (effects.mask)
        maskFactor = maskFactors(*effects.mask, surface.width, surface.height, toPixels);

    const auto width = static_cast<std::size_t>(surface.width);
    const auto isolatedWidth = static_cast<std::size_t>(isolated.width);
    for (std::size_t i = 0; i < surface.pixels.size(); ++i) {
        const std::size_t x = i % width + static_cast<std::size_t>(left);
        const std::size_t y = i / width + static_cast<std::size_t>(top);
        const Pixel &p = isolated.pixels[y * isolatedWidth + x];
        double factor = effects.opacity;
        if (effects.clip)
            factor *= static_cast<double>(clipCover[i]);
        if (effects.mask)
            factor *= maskFactor[i];
        if (p.a > 0.0 && factor > 0.0)
            over(surface.pixels[i], {p.r * factor, p.g * factor, p.b * factor, p.a * factor});
    }
}

// Composites @p layer's fill and then its stroke over @p surface, each with its alpha scaled by @p opacity. @p toPixels
// maps the path's coordinates to pixels.
void paint(Surface &surface, const Layer &layer, const Affine &toPixels, double opacity) {
    const Affine toPaint = inverse(toPixels);
    if (layer.fill) {
        const std::vector<Contour> outline = flatten(transformed(layer.path, toPixels), renderTolerance);
        composite(surface, coverage(outline, surface.width, surface.height, layer.fillRule),
                  detail::Shader(*layer.fill, toPixels), toPaint, opacity);
    }
    const double stretch = maxScale(toPixels);
    if (layer.stroke && stretch > 0.0) {
        // The stroke is built in the path's own coordinates, so a transform widens it as it widens the path, at a
        // tolerance that stays renderTolerance once the transform scales it.
        const double tolerance = renderTolerance / stretch;
        const std::vector<Contour> pen =
            mapped(strokeOutline(flatten(layer.path, tolerance), layer.strokeStyle, tolerance), toPixels);
        composite(surface, coverage(pen, surface.width, surface.height, FillRule::nonZero),
                  detail::Shader(*layer.stroke, toPixels), toPaint, opacity);
    }
}

void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent);

// Draws @p layer on @p surface. @p parent maps the coordinates the layer sits in to pixels.
void draw(Surface &surface, const Layer &layer, const Affine &parent) {
    const double opacity = std::clamp(layer.opacity, 0.0, 1.0);
    if (!(opacity > 0.0) || layer.path.empty())
        return;
    const Affine toPixels = parent * layer.transform;
    const Effects effects{layer.blur, held(layer.shadow), held(layer.clip), nullptr, opacity};
    if (!effects.any() && (opacity >= 1.0 || !layer.fill || !layer.stroke)) {
        // At full opacity nothing fades. A fill or a stroke alone is one coverage composited once, so scaling its alpha
        // by the opacity gives exactly what fading it on a surface of its own would, without that surface.
        paint(surface, layer, toPixels, opacity);
        return;
    }
    drawIsolated(surface, toPixels, effects,
                 [&](Surface &isolated, const Affine &toIsolated) { paint(isolated, layer, toIsolated, 1.0); });
}

// Draws @p group on @p surface. @p parent maps the coordinates the group sits in to pixels.
void draw(Surface &surface, const Group &group, const Affine &parent) {
    const double opacity = std::clamp(group.opacity, 0.0, 1.0);
    if (!(opacity > 0.0) || group.children.empty())
        return;
    const Affine toPixels = parent * group.transform;
    const Effects effects{group.blur, held(group.shadow), held(group.clip), held(group.mask), opacity};
    if (!effects.any() && opacity >= 1.0) {
        // Compositing over is associative, so drawing the children one by one onto the surface gives what drawing them
        // on a surface of their own and compositing that would, without the second surface.
        drawNodes(surface, group.children, toPixels);
        return;
    }
    drawIsolated(surface, toPixels, effects,
                 [&](Surface &isolated, const Affine &toIsolated) { drawNodes(isolated, group.children, toIsolated); });
}

// Draws @p nodes on @p surface in order. @p parent maps the coordinates the nodes sit in to pixels.
void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent) {
    for (const Node &node : nodes)
        std::visit([&](const auto &content) { draw(surface, content, parent); }, node.content);
}

} // namespace

namespace detail {

std::vector<Pixel> draw(const Drawing &drawing, int width, int height) {
    Surface surface{width, height,
                    std::vector<Pixel>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height))};
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
