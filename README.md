# linea

A pure C++20 vector graphics library. Describe shapes as paths of lines, Bézier curves and arcs, fill and stroke them
with solid colors, gradients or patterns, and render them anti-aliased at any size. No image files, no GPU, no
dependencies beyond the standard library.

An `Image` is plain data, `{ int width, height; std::vector<std::uint8_t> rgba }`: RGBA, row-major, origin at the top
left, sRGB-encoded with straight alpha, as PNG files store it. It uploads straight to SDL (`SDL_PIXELFORMAT_RGBA32`)
or any other backend.

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

linea reads and writes no files. `encodePng` gives the bytes of a PNG file, to write wherever they belong:

```cpp
#include <linea/Png.hpp>

#include <fstream>

void save(const linea::Image &image, const char *path) {
    const std::vector<std::uint8_t> png = linea::encodePng(image);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(png.data()),
                                                static_cast<std::streamsize>(png.size()));
}
```

## Modules

| Header | Provides |
| --- | --- |
| `Geometry.hpp` | Points, affine transforms (translation, rotation, scale, skew), composition and inverses |
| `Path.hpp` | Paths of lines, quadratic and cubic Bézier curves and elliptical arcs, with SVG's arc semantics; reversing them, and their control-point and tight bounds |
| `Shapes.hpp` | Circles, ellipses, rectangles, rounded rectangles, polygons, stars and rings as paths |
| `Flatten.hpp` | Bézier evaluation and splitting, flattening paths into polylines within a tolerance, and simplifying polylines with Ramer-Douglas-Peucker |
| `Measure.hpp` | Arc length by adaptive Gauss-Legendre quadrature, the point and tangent at a distance along a path, and splitting a path there |
| `Stroke.hpp` | Stroke outlines with miter, round and bevel joins, butt, round and square caps, and SVG's dash patterns |
| `Noise.hpp` | Seeded Perlin, simplex and fractal noise, Voronoi cells, and paths wobbled into organic outlines |
| `Raster.hpp` | Exact-area anti-aliased coverage under the nonzero and even-odd fill rules |
| `Boolean.hpp` | Union, intersection, difference and exclusive or of paths, under either fill rule |
| `Color.hpp` | Colors from hex, HSL, Oklab, Oklch and the 148 CSS names, and lighten, darken, saturate, desaturate and mix in Oklab |
| `Paint.hpp` | Solid colors; linear, radial and conic gradients with SVG's pad, repeat and reflect, focal circles as the HTML canvas draws them, and blending in Oklab, sRGB or linear light; and patterns that tile a drawing |
| `Indirect.hpp` | A value held in storage of its own that copies and compares as the value itself, as C++26's `std::indirect`, so that a group can hold nodes that hold groups |
| `Drawing.hpp` | Layers of filled and stroked paths and groups of them, each with its own transform, opacity, clip path, Gaussian blur and drop shadow, groups with alpha or luminance masks, composited in linear light |
| `Patterns.hpp` | Dot grids, line grids, crosshatching and checkerboards as ready-made patterns |
| `Text.hpp` | TrueType fonts read with no dependency, and lines of text as paths with the font's advances and `kern` table kerning |
| `Png.hpp` | Images as PNG files, compressed losslessly, with no dependency |
| `Svg.hpp` | Drawings without clips, masks, blurs, drop shadows, patterns or conic gradients written as SVG documents that read back exactly, and SVG documents read as drawings, refusing what linea cannot draw as a browser would; SVG path data both ways |

Gradients blend in Oklab by default; set a gradient's `interpolation` to `Interpolation::srgb` to blend as SVG and CSS
do. A group composites its children as one image, and a layer its fill and stroke, as SVG's opacity does. Noise is a
pure function of a point and a seed, the same on every platform to within rounding. The SVG reader refuses entity
declarations and bounds nesting and what `use` elements may copy, so a document from anywhere is safe to read.

## Requirements

linea needs CMake 3.21 or newer and a C++20 compiler. On Apple platforms, the deployment target must be macOS 13.3, iOS
16.3, tvOS 16.3 or watchOS 9.3 or later, the first whose libc++ has floating-point `std::to_chars`. CI builds and tests
with GCC and Clang on Linux, MSVC on Windows and Apple Clang on macOS.

## Use it from CMake

linea has no tagged releases, so pin a commit of `main`: every configure then builds the same linea, without fetching
to check for changes.

```cmake
include(FetchContent)
FetchContent_Declare(linea GIT_REPOSITORY https://github.com/Gondola-Bros-Entertainment/linea.git GIT_TAG <commit>)
FetchContent_MakeAvailable(linea)
target_link_libraries(my_game PRIVATE linea::linea)
```

`add_subdirectory` on a checkout works the same way.

linea always builds as a static library, even with `BUILD_SHARED_LIBS` on, so a project that links it into a shared
library turns `CMAKE_POSITION_INDEPENDENT_CODE` on before adding it.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

With CMake 3.25 or newer and Ninja, the `debug`, `release` and `sanitizers` workflow presets each configure, build and
test in one step. `sanitizers` builds with AddressSanitizer and UndefinedBehaviorSanitizer, which needs GCC or Clang:

```sh
cmake --workflow --preset sanitizers
```

The tests run as two CTest tests: `linea_fast_tests`, and `linea_slow_tests`, labeled `slow`, which holds the
exhaustive sweeps and randomized stress cases and takes most of the time. `ctest -LE slow` runs the fast set alone; CI
runs both.

| Option | Default | Effect |
| --- | --- | --- |
| `LINEA_BUILD_TESTS` | On when linea is the top-level project | Builds the doctest suite in `tests/` |
| `LINEA_WARNINGS_AS_ERRORS` | On when linea is the top-level project | Fails the build on any compiler warning |

Formatting follows `.clang-format` (clang-format 22).

## License

MIT. See [LICENSE](LICENSE).
