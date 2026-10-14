// linea - Shading implementation: gradients sampled in premultiplied Oklab.
#include "Shading.hpp"

#include <algorithm>
#include <variant>

namespace linea::detail {

Shader::Shader(const Paint &paint) {
    const auto prepare = [&](const std::vector<GradientStop> &stops) {
        for (const GradientStop &stop : stops) {
            const Oklab lab = toOklab(stop.color);
            stops_.push_back({std::clamp(stop.offset, 0.0, 1.0), lab.l * lab.alpha, lab.a * lab.alpha,
                              lab.b * lab.alpha, lab.alpha});
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
        mixed = {t, left.l + (right.l - left.l) * u, left.a + (right.a - left.a) * u, left.b + (right.b - left.b) * u,
                 left.alpha + (right.alpha - left.alpha) * u};
    }
    if (mixed.alpha <= 0.0)
        return {};
    const LinearColor c =
        oklabToLinear({mixed.l / mixed.alpha, mixed.a / mixed.alpha, mixed.b / mixed.alpha, mixed.alpha});
    // A blend of colors inside the sRGB gamut can stray just outside it, so each channel clips back to it.
    return {std::clamp(c.r, 0.0, 1.0), std::clamp(c.g, 0.0, 1.0), std::clamp(c.b, 0.0, 1.0), c.a};
}

} // namespace linea::detail
