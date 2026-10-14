#include "linea/Color.hpp"
#include "linea/Drawing.hpp"
#include "linea/Flatten.hpp"
#include "linea/Geometry.hpp"
#include "linea/Measure.hpp"
#include "linea/Noise.hpp"
#include "linea/Paint.hpp"
#include "linea/Path.hpp"
#include "linea/Raster.hpp"
#include "linea/Stroke.hpp"
#include "linea/Svg.hpp"

#include <doctest/doctest.h>

using namespace linea;

// Each public aggregate below is initialized naming only its first member and naming only its last, so that every
// member is left out at least once. GCC's and Clang's -Wextra warn of a member left out of a designated initializer
// unless it has a default member initializer, and linea builds its tests with warnings as errors, so this file
// compiling is the check. The checks below confirm that what is left out takes its default.
TEST_CASE("Every public aggregate takes a designated initializer that leaves out any of its members") {
    CHECK(Color{.r = 0} == Color{.a = 255});
    CHECK(Hsl{.h = 0.0}.alpha == Hsl{.alpha = 1.0}.alpha);
    CHECK(Oklab{.l = 0.0}.alpha == Oklab{.alpha = 1.0}.alpha);
    CHECK(Oklch{.l = 0.0}.alpha == Oklch{.alpha = 1.0}.alpha);
    CHECK(NamedColor{.name = ""}.color == NamedColor{.color = {}}.color);
    CHECK(Vec2{.x = 0.0} == Vec2{.y = 0.0});
    CHECK(Affine{.a = 1.0} == Affine{.f = 0.0});
    CHECK(Contour{.points = {}}.closed == Contour{.closed = false}.closed);
    CHECK(PathPoint{.point = {}}.tangent == PathPoint{.tangent = {}}.point);
    CHECK(PathParts{.before = {}}.after == PathParts{.after = {}}.before);
    CHECK(Bounds{.min = {}}.max == Bounds{.max = {}}.min);
    CHECK(Fractal{.octaves = 4}.gain == Fractal{.gain = 0.5}.gain);
    CHECK(Voronoi{.nearest = 0.0}.id == Voronoi{.id = 0}.id);
    CHECK(Wobble{.amplitude = 0.0}.seed == Wobble{.seed = 0}.seed);
    CHECK(Image{.width = 0}.rgba == Image{.rgba = {}}.rgba);
    CHECK(StrokeStyle{.width = 1.0} == StrokeStyle{.dashOffset = 0.0});
    CHECK(GradientStop{.offset = 0.0} == GradientStop{.color = {}});
    CHECK(LinearGradient{.from = {}} == LinearGradient{.interpolation = Interpolation::oklab});
    CHECK(RadialGradient{.center = {}} == RadialGradient{.interpolation = Interpolation::oklab});
    CHECK(ConicGradient{.center = {}} == ConicGradient{.interpolation = Interpolation::oklab});
    CHECK(Pattern{.tile = {}} == Pattern{.transform = {}});
    CHECK(Clip{.path = {}} == Clip{.fillRule = FillRule::nonZero});
    CHECK(DropShadow{.offset = {}} == DropShadow{.color = {}});
    CHECK(Layer{.path = {}} == Layer{.clip = {}});
    CHECK(Mask{.nodes = {}} == Mask{.mode = MaskMode::alpha});
    CHECK(Group{.children = {}} == Group{.mask = {}});
    CHECK(Drawing{.width = 0.0} == Drawing{.nodes = {}});
    CHECK(SvgDocument{.drawing = {}}.errors.empty());
    CHECK(SvgDocument{.errors = {}}.drawing == Drawing{});
    CHECK_FALSE(SvgPathData{.path = {}}.error);
    CHECK(SvgPathData{.error = {}}.path == Path{});
}
