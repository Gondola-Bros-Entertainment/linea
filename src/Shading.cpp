// linea - Shading implementation: gradients sampled in premultiplied Oklab.
#include "Shading.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace linea::detail {
namespace {

// Linear sRGB to and from Oklab, with the matrices Bjorn Ottosson published with the space (2020).
struct Lab {
    double l{};
    double a{};
    double b{};
};

Lab toOklab(double r, double g, double b) {
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

LinearColor fromOklab(Lab c, double alpha) {
    const double l = c.l + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m = c.l - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s = c.l - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l3 = l * l * l;
    const double m3 = m * m * m;
    const double s3 = s * s * s;
    return {std::clamp(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3, 0.0, 1.0),
            std::clamp(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3, 0.0, 1.0),
            std::clamp(-0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3, 0.0, 1.0), alpha};
}

constexpr double byteMax = 255.0;

LinearColor linear(Color c) {
    return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), static_cast<double>(c.a) / byteMax};
}

} // namespace

Shader::Shader(const Paint &paint) {
    const auto prepare = [&](const std::vector<GradientStop> &stops) {
        for (const GradientStop &stop : stops) {
            const LinearColor c = linear(stop.color);
            const Lab lab = toOklab(c.r, c.g, c.b);
            stops_.push_back({std::clamp(stop.offset, 0.0, 1.0), lab.l * c.a, lab.a * c.a, lab.b * c.a, c.a});
        }
        std::stable_sort(stops_.begin(), stops_.end(),
                         [](const Stop &left, const Stop &right) { return left.offset < right.offset; });
    };
    if (const auto *color = std::get_if<Color>(&paint)) {
        solid_ = linear(*color);
    } else if (const auto *gradient = std::get_if<LinearGradient>(&paint)) {
        kind_ = Kind::linear;
        from_ = gradient->from;
        axis_ = gradient->to - gradient->from;
        axisLength2_ = dot(axis_, axis_);
        prepare(gradient->stops);
    } else if (const auto *radial = std::get_if<RadialGradient>(&paint)) {
        kind_ = Kind::radial;
        from_ = radial->center;
        radius_ = radial->radius;
        prepare(radial->stops);
    }
}

LinearColor Shader::at(Vec2 p) const {
    switch (kind_) {
    case Kind::solid:
        return solid_;
    case Kind::linear:
        return along(axisLength2_ > 0.0 ? dot(p - from_, axis_) / axisLength2_ : 0.0);
    case Kind::radial:
        return along(radius_ > 0.0 ? length(p - from_) / radius_ : 0.0);
    }
    return solid_;
}

LinearColor Shader::along(double t) const {
    if (stops_.empty())
        return {};
    const Stop *chosen = &stops_.front();
    Lab lab{chosen->l, chosen->a, chosen->b};
    double alpha = chosen->alpha;
    if (t >= stops_.back().offset) {
        chosen = &stops_.back();
        lab = {chosen->l, chosen->a, chosen->b};
        alpha = chosen->alpha;
    } else if (t > stops_.front().offset) {
        const auto after = std::upper_bound(stops_.begin(), stops_.end(), t,
                                            [](double offset, const Stop &stop) { return offset < stop.offset; });
        const Stop &left = *(after - 1);
        const Stop &right = *after;
        const double span = right.offset - left.offset;
        const double u = span > 0.0 ? (t - left.offset) / span : 1.0;
        lab = {left.l + (right.l - left.l) * u, left.a + (right.a - left.a) * u, left.b + (right.b - left.b) * u};
        alpha = left.alpha + (right.alpha - left.alpha) * u;
    }
    if (alpha <= 0.0)
        return {};
    return fromOklab({lab.l / alpha, lab.a / alpha, lab.b / alpha}, alpha);
}

} // namespace linea::detail
