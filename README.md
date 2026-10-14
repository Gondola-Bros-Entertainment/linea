# linea

A pure C++20 vector graphics library. Describe shapes as paths of lines, Bézier curves and arcs, fill and stroke them
with solid colors or gradients, and render them anti-aliased at any size. No image files, no GPU, no dependencies
beyond the standard library.

An `Image` is plain data, `{ int width, height; std::vector<std::uint8_t> rgba }`: RGBA, row-major, origin at the top
left, sRGB-encoded with straight alpha, as PNG files store it. It shares tessera's `Canvas` layout and uploads straight
to SDL (`SDL_PIXELFORMAT_RGBA32`) or any other backend. Everything is a free function over plain structs.

## Example

```cpp
#include <linea/Drawing.hpp>
#include <linea/Shapes.hpp>

#include <numbers>

linea::Image badge(int pixels) {
    using namespace linea;
    Layer gem;
    gem.path = star({50.0, 50.0}, 44.0, 20.0, 5, -std::numbers::pi / 2.0);
    gem.fill = RadialGradient{{50.0, 40.0}, 48.0, evenStops({{255, 236, 160, 255}, {214, 120, 24, 255}})};
    gem.stroke = Paint{Color{40, 24, 12, 255}};
    gem.strokeStyle = {.width = 4.0, .join = Join::round};
    return render(Drawing{100.0, 100.0, {gem}}, pixels, pixels);
}
```

The drawing is 100 by 100 units, and `render` draws it at whatever size it is asked for, so the same badge is sharp at
32 pixels and at 1024.

## Modules

| Header | Provides |
| --- | --- |
| `Geometry.hpp` | Points, affine transforms (translation, rotation, scale, skew), composition and inverses |
| `Path.hpp` | Paths of lines, quadratic and cubic Bézier curves and elliptical arcs, with SVG's arc semantics; reversing them, and their control-point and tight bounds |
| `Shapes.hpp` | Circles, ellipses, rectangles, rounded rectangles, polygons, stars and rings as paths |
| `Flatten.hpp` | Bézier evaluation and splitting, flattening paths into polylines within a tolerance, and simplifying polylines with Ramer-Douglas-Peucker |
| `Measure.hpp` | Arc length by adaptive Gauss-Legendre quadrature, the point and tangent at a distance along a path, and splitting a path there |
| `Stroke.hpp` | Stroke outlines with miter, round and bevel joins, butt, round and square caps, and SVG's dash patterns |
| `Raster.hpp` | Exact-area anti-aliased coverage under the nonzero and even-odd fill rules |
| `Color.hpp` | Colors from hex, HSL, Oklab, Oklch and the 148 CSS names, and lighten, darken, saturate, desaturate and mix in Oklab |
| `Paint.hpp` | Solid colors, and linear and radial gradients that blend in Oklab |
| `Drawing.hpp` | Layers of filled and stroked paths, and groups of them with their own transform and opacity, composited in linear light |

Coverage is the exact area of each pixel that a shape covers, so edges are smooth at every size without
supersampling. Gradients blend in Oklab, a perceptual color space, so a gradient between two saturated colors does
not pass through grey. Color adjustments work in Oklab too, so lightening any color by 0.1 raises its perceived
lightness by the same step. Layers composite in linear light, as light adds up. A group composites its children as
one image, and a layer its fill and stroke, as SVG's opacity does, so a translucent group or layer fades as a whole
without its parts showing through each other.

## Use it from CMake

```cmake
include(FetchContent)
FetchContent_Declare(linea GIT_REPOSITORY https://github.com/Gondola-Bros-Entertainment/linea.git GIT_TAG main)
FetchContent_MakeAvailable(linea)
target_link_libraries(my_game PRIVATE linea::linea)
```

`add_subdirectory` on a checkout works the same way.

## Build and test

Requires CMake 3.21 or newer and a C++20 compiler. CI builds and tests with GCC and Clang on Linux, MSVC on Windows and
Apple Clang on macOS, and runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | Effect |
| --- | --- | --- |
| `LINEA_BUILD_TESTS` | On when linea is the top-level project | Builds the doctest suite in `tests/` |
| `LINEA_WARNINGS_AS_ERRORS` | On | Fails the build on any compiler warning |

Formatting follows `.clang-format` (clang-format 22).

## License

MIT. See [LICENSE](LICENSE).
