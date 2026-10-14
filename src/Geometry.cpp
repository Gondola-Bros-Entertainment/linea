// linea - Geometry implementation.
#include "linea/Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace linea {

Affine inverse(const Affine &m) {
    const double det = determinant(m);
    if (det == 0.0 || !std::isfinite(det))
        return identity();
    const double inv = 1.0 / det;
    const double a = m.d * inv;
    const double b = -m.b * inv;
    const double c = -m.c * inv;
    const double d = m.a * inv;
    return {a, b, c, d, -(a * m.e + c * m.f), -(b * m.e + d * m.f)};
}

double maxScale(const Affine &m) {
    // The singular values of [[a c] [b d]] are the square roots of the eigenvalues of its Gram matrix.
    const double p = m.a * m.a + m.b * m.b;
    const double q = m.a * m.c + m.b * m.d;
    const double r = m.c * m.c + m.d * m.d;
    const double mean = 0.5 * (p + r);
    const double spread = std::sqrt(std::max(0.0, 0.25 * (p - r) * (p - r) + q * q));
    return std::sqrt(mean + spread);
}

} // namespace linea
