// linea - Drawing implementation: rasterize each layer's fill and stroke, then composite them over the image in
// linear light with premultiplied alpha. A translucent group, or a translucent layer with both a fill and a stroke,
// draws on a surface of its own first, then composites that surface over the image as one.
#include "linea/Drawing.hpp"

#include "Shading.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>

namespace linea {
namespace {

constexpr double byteMax = 255.0;

// Linear-light color with premultiplied alpha, one per pixel.
struct Pixel {
    double r{};
    double g{};
    double b{};
    double a{};
};

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

// Calls @p drawOn with a transparent surface the size of @p surface, then composites what it drew over @p surface as
// one image, its alpha scaled by @p opacity.
void drawIsolated(Surface &surface, double opacity, const std::invocable<Surface &> auto &drawOn) {
    Surface isolated{surface.width, surface.height, std::vector<Pixel>(surface.pixels.size())};
    drawOn(isolated);
    for (std::size_t i = 0; i < isolated.pixels.size(); ++i) {
        const Pixel &p = isolated.pixels[i];
        if (p.a > 0.0)
            over(surface.pixels[i], {p.r * opacity, p.g * opacity, p.b * opacity, p.a * opacity});
    }
}

// Composites @p layer's fill and then its stroke over @p surface, each with its alpha scaled by @p opacity. @p toPixels
// maps the path's coordinates to pixels.
void paint(Surface &surface, const Layer &layer, const Affine &toPixels, double opacity) {
    const Affine toPaint = inverse(toPixels);
    if (layer.fill) {
        const std::vector<Contour> outline = flatten(transformed(layer.path, toPixels), renderTolerance);
        composite(surface, coverage(outline, surface.width, surface.height, layer.fillRule),
                  detail::Shader(*layer.fill), toPaint, opacity);
    }
    const double stretch = maxScale(toPixels);
    if (layer.stroke && stretch > 0.0) {
        // The stroke is built in the path's own coordinates, so a transform widens it as it widens the path, at a
        // tolerance that stays renderTolerance once the transform scales it.
        const double tolerance = renderTolerance / stretch;
        const std::vector<Contour> pen =
            mapped(strokeOutline(flatten(layer.path, tolerance), layer.strokeStyle, tolerance), toPixels);
        composite(surface, coverage(pen, surface.width, surface.height, FillRule::nonZero),
                  detail::Shader(*layer.stroke), toPaint, opacity);
    }
}

void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent);

// Draws @p layer on @p surface. @p parent maps the coordinates the layer sits in to pixels.
void draw(Surface &surface, const Layer &layer, const Affine &parent) {
    const double opacity = std::clamp(layer.opacity, 0.0, 1.0);
    if (!(opacity > 0.0) || layer.path.empty())
        return;
    const Affine toPixels = parent * layer.transform;
    if (opacity >= 1.0 || !layer.fill || !layer.stroke) {
        // At full opacity nothing fades. A fill or a stroke alone is one coverage composited once, so scaling its alpha
        // by the opacity gives exactly what fading it on a surface of its own would, without that surface.
        paint(surface, layer, toPixels, opacity);
        return;
    }
    drawIsolated(surface, opacity, [&](Surface &isolated) { paint(isolated, layer, toPixels, 1.0); });
}

// Draws @p group on @p surface. @p parent maps the coordinates the group sits in to pixels.
void draw(Surface &surface, const Group &group, const Affine &parent) {
    const double opacity = std::clamp(group.opacity, 0.0, 1.0);
    if (!(opacity > 0.0) || group.children.empty())
        return;
    const Affine toPixels = parent * group.transform;
    if (opacity >= 1.0) {
        // Compositing over is associative, so drawing the children one by one onto the surface gives what drawing them
        // on a surface of their own and compositing that would, without the second surface.
        drawNodes(surface, group.children, toPixels);
        return;
    }
    drawIsolated(surface, opacity, [&](Surface &isolated) { drawNodes(isolated, group.children, toPixels); });
}

// Draws @p nodes on @p surface in order. @p parent maps the coordinates the nodes sit in to pixels.
void drawNodes(Surface &surface, const std::vector<Node> &nodes, const Affine &parent) {
    for (const Node &node : nodes)
        std::visit([&](const auto &content) { draw(surface, content, parent); }, node.content);
}

} // namespace

Image render(const Drawing &drawing, int pixelWidth, int pixelHeight) {
    const int width = std::max(1, pixelWidth);
    const int height = std::max(1, pixelHeight);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    Image image{width, height, std::vector<std::uint8_t>(count * 4, 0)};
    if (!(drawing.width > 0.0) || !(drawing.height > 0.0))
        return image;

    Surface surface{width, height, std::vector<Pixel>(count)};
    drawNodes(surface, drawing.nodes, scale(width / drawing.width, height / drawing.height));

    for (std::size_t i = 0; i < count; ++i) {
        const Pixel &p = surface.pixels[i];
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
