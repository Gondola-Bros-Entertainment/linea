// linea - Drawing implementation: rasterize each layer's fill and stroke, then composite them over the image in
// linear light with premultiplied alpha.
#include "linea/Drawing.hpp"

#include "Shading.hpp"

#include <algorithm>
#include <cmath>
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

// Composites @p shader over @p image wherever @p cover covers it, sampling the paint at each pixel's center mapped
// back through @p toPaint.
void composite(std::vector<Pixel> &image, int width, const std::vector<float> &cover, const detail::Shader &shader,
               const Affine &toPaint, double opacity) {
    for (std::size_t i = 0; i < cover.size(); ++i) {
        if (cover[i] <= 0.0f)
            continue;
        const auto x = static_cast<double>(i % static_cast<std::size_t>(width));
        const auto y = static_cast<double>(i / static_cast<std::size_t>(width));
        const detail::LinearColor color = shader.at(apply(toPaint, {x + 0.5, y + 0.5}));
        const double alpha = color.a * static_cast<double>(cover[i]) * opacity;
        if (alpha <= 0.0)
            continue;
        Pixel &dst = image[i];
        const double keep = 1.0 - alpha;
        dst = {color.r * alpha + dst.r * keep, color.g * alpha + dst.g * keep, color.b * alpha + dst.b * keep,
               alpha + dst.a * keep};
    }
}

std::vector<Contour> mapped(std::vector<Contour> contours, const Affine &m) {
    for (Contour &contour : contours)
        for (Vec2 &p : contour.points)
            p = apply(m, p);
    return contours;
}

} // namespace

Image render(const Drawing &drawing, int pixelWidth, int pixelHeight) {
    const int width = std::max(1, pixelWidth);
    const int height = std::max(1, pixelHeight);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    Image image{width, height, std::vector<std::uint8_t>(count * 4, 0)};
    if (!(drawing.width > 0.0) || !(drawing.height > 0.0))
        return image;

    std::vector<Pixel> pixels(count);
    const Affine view = scale(width / drawing.width, height / drawing.height);
    for (const Layer &layer : drawing.layers) {
        const double opacity = std::clamp(layer.opacity, 0.0, 1.0);
        if (opacity <= 0.0 || layer.path.empty())
            continue;
        const Affine toPixels = view * layer.transform;
        const Affine toPaint = inverse(toPixels);
        if (layer.fill) {
            const std::vector<Contour> outline = flatten(transformed(layer.path, toPixels), renderTolerance);
            composite(pixels, width, coverage(outline, width, height, layer.fillRule), detail::Shader(*layer.fill),
                      toPaint, opacity);
        }
        const double stretch = maxScale(toPixels);
        if (layer.stroke && stretch > 0.0) {
            // The stroke is built in the path's own coordinates, so a transform widens it as it widens the path, at a
            // tolerance that stays renderTolerance once the transform scales it.
            const double tolerance = renderTolerance / stretch;
            const std::vector<Contour> pen =
                mapped(strokeOutline(flatten(layer.path, tolerance), layer.strokeStyle, tolerance), toPixels);
            composite(pixels, width, coverage(pen, width, height, FillRule::nonZero), detail::Shader(*layer.stroke),
                      toPaint, opacity);
        }
    }

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
