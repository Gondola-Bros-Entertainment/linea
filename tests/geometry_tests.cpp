#include "linea/Geometry.hpp"

#include <doctest/doctest.h>

#include <numbers>

using namespace linea;

namespace {

void checkNear(Vec2 a, Vec2 b) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-12));
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-12));
}

} // namespace

TEST_CASE("A composed transform applies the right-hand one first") {
    const Affine moveThenScale = scale(2.0) * translate({3.0, 1.0});
    checkNear(apply(moveThenScale, {1.0, 1.0}), {8.0, 4.0});
    const Affine scaleThenMove = translate({3.0, 1.0}) * scale(2.0);
    checkNear(apply(scaleThenMove, {1.0, 1.0}), {5.0, 3.0});
}

TEST_CASE("Positive rotations turn clockwise on a y-down canvas") {
    checkNear(apply(rotate(std::numbers::pi / 2.0), {1.0, 0.0}), {0.0, 1.0});
    checkNear(apply(rotateAround(std::numbers::pi, {5.0, 5.0}), {6.0, 5.0}), {4.0, 5.0});
}

TEST_CASE("The inverse undoes a transform, and a collapsed one has none") {
    const Affine m = translate({4.0, -2.0}) * rotate(0.7) * scale(3.0, 0.5) * skewX(0.2);
    const Vec2 p{2.5, -7.0};
    checkNear(apply(inverse(m), apply(m, p)), p);
    checkNear(apply(m * inverse(m), p), p);
    CHECK(inverse(scale(0.0, 1.0)) == identity());
}

TEST_CASE("maxScale finds the greatest stretch of a transform") {
    CHECK(maxScale(rotate(1.1) * scale(3.0, 2.0)) == doctest::Approx(3.0));
    CHECK(maxScale(translate({50.0, 50.0})) == doctest::Approx(1.0));
    CHECK(maxScale(skewX(std::numbers::pi / 4.0)) == doctest::Approx((1.0 + std::sqrt(5.0)) / 2.0));
}
