// linea - Patterns implementation: each pattern is one square tile. Every shape of one color shares a path, so that
// where shapes overlap or meet within a pixel, the nonzero rule covers their union exactly once.
#include "linea/Patterns.hpp"

#include "linea/Shapes.hpp"

#include <array>
#include <memory>
#include <numbers>
#include <utility>

namespace linea {
namespace {

Layer filled(Path path, Color color) {
    Layer layer;
    layer.path = std::move(path);
    layer.fill = color;
    return layer;
}

// A pattern whose tile, @p side units square, holds @p path filled with @p color.
Pattern tiled(double side, Path path, Color color) {
    return {std::make_shared<const Drawing>(Drawing{side, side, {filled(std::move(path), color)}})};
}

} // namespace

Pattern dotGrid(double spacing, double radius, Color color) {
    // Each corner of the tile holds a quarter of a dot. Every other dot is centered a whole spacing or more from the
    // tile, so it reaches in only with a radius over spacing, and every point of the tile lies within half its
    // diagonal, spacing / sqrt(2), of a corner, so the corners' dots then cover the whole tile already.
    Path dots;
    if (radius > 0.0)
        for (const Vec2 corner : {Vec2{0.0, 0.0}, Vec2{spacing, 0.0}, Vec2{0.0, spacing}, Vec2{spacing, spacing}})
            dots.append(circle(corner, radius));
    return tiled(spacing, std::move(dots), color);
}

Pattern lineGrid(double spacing, double width, Color color) {
    // The lines along the tile's edges are the nearest of their direction to every point of it, so those four, each
    // with half its width inside, draw all of it.
    Path lines;
    if (width > 0.0) {
        const double half = width / 2.0;
        lines.append(rectangle({0.0, 0.0}, {half, spacing}))
            .append(rectangle({spacing - half, 0.0}, {spacing, spacing}))
            .append(rectangle({0.0, 0.0}, {spacing, half}))
            .append(rectangle({0.0, spacing - half}, {spacing, spacing}));
    }
    return tiled(spacing, std::move(lines), color);
}

Pattern crosshatch(double spacing, double width, Color color) {
    // Every point of the tile that some line's band covers, the band of one of the tile's diagonals, x - y = 0 and
    // x + y = spacing, covers too: the next lines out, through the tile's corners, reach in only within the band of the
    // diagonal through the same corner, and any line farther out reaches in only when the bands are so wide that each
    // diagonal's covers the whole tile. Each band, @p width wide across its line, spans reach either side of it along
    // x, from the tile's top edge to its bottom.
    Path lines;
    if (width > 0.0) {
        const double reach = width * std::numbers::sqrt2 / 2.0;
        const std::array<Vec2, 4> down{
            {{-reach, 0.0}, {reach, 0.0}, {spacing + reach, spacing}, {spacing - reach, spacing}}};
        const std::array<Vec2, 4> up{
            {{spacing - reach, 0.0}, {spacing + reach, 0.0}, {reach, spacing}, {-reach, spacing}}};
        lines.append(polygon(down)).append(polygon(up));
    }
    return tiled(spacing, std::move(lines), color);
}

Pattern checkerboard(double size, Color first, Color second) {
    const double side = 2.0 * size;
    Path squares = rectangle({0.0, 0.0}, {size, size});
    squares.append(rectangle({size, size}, {side, side}));
    Layer background = filled(rectangle({0.0, 0.0}, {side, side}), second);
    return {std::make_shared<const Drawing>(
        Drawing{side, side, {std::move(background), filled(std::move(squares), first)}})};
}

} // namespace linea
