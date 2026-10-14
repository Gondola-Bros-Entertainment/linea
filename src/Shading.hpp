// linea - shading paints at points, for the renderer. Internal.
#pragma once

#include "ColorSpace.hpp"
#include "linea/Paint.hpp"

#include <vector>

namespace linea::detail {

// A paint ready to sample: gradient stops sorted and converted to premultiplied Oklab once.
class Shader {
  public:
    explicit Shader(const Paint &paint);
    // The paint's color at @p p, in the paint's own coordinates.
    [[nodiscard]] LinearColor at(Vec2 p) const;

  private:
    struct Stop {
        double offset{};
        // Oklab lightness and opponent axes, each multiplied by alpha, then alpha.
        double l{};
        double a{};
        double b{};
        double alpha{};
    };
    enum class Kind { solid, linear, radial };

    [[nodiscard]] LinearColor along(double t) const;

    Kind kind_ = Kind::solid;
    LinearColor solid_{};
    Vec2 from_{};
    Vec2 axis_{};
    double axisLength2_{};
    double radius_{};
    std::vector<Stop> stops_;
};

} // namespace linea::detail
