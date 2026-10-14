// linea - roots of polynomials between 0 and 1, where curves keep their parameters. Internal.
#pragma once

#include <cmath>

namespace linea::detail {

// Calls @p visit with each root of a t^2 + b t + c strictly between 0 and 1. The quadratic formula takes the form that
// never subtracts nearly equal numbers, so neither root loses precision to cancellation, and a of 0 leaves a linear
// equation.
template <typename Visit> void forEachUnitRoot(double a, double b, double c, Visit visit) {
    const auto inside = [&](double t) {
        if (t > 0.0 && t < 1.0)
            visit(t);
    };
    if (a == 0.0) {
        if (b != 0.0)
            inside(-c / b);
        return;
    }
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0)
        return;
    const double q = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
    inside(q / a);
    // q is 0 only for the double root t = 0, which lies outside.
    if (q != 0.0)
        inside(c / q);
}

} // namespace linea::detail
