// linea - ready-made patterns: dot grids, line grids, crosshatching and checkerboards.
#pragma once

#include "linea/Color.hpp"
#include "linea/Drawing.hpp"

namespace linea {

// Each pattern below repeats a square tile with its corner at the origin and the identity transform, so that its
// dots, lines and squares line up on whole multiples of its spacing in the coordinates of the path it paints. Set the
// pattern's transform to move, turn or scale them. A spacing or size of 0 or less paints nothing, and so does a radius
// or width of 0 or less.

// Dots of @p radius in @p color, centered on every point whose coordinates are whole multiples of @p spacing.
Pattern dotGrid(double spacing, double radius, Color color);

// Lines @p width wide in @p color, centered on every line x = k spacing and y = k spacing for a whole number k.
Pattern lineGrid(double spacing, double width, Color color);

// Lines @p width wide in @p color at 45 degrees both ways, centered on every line x - y = k spacing and
// x + y = k spacing for a whole number k: through every point whose coordinates are whole multiples of @p spacing, the
// lines of each direction spacing / sqrt(2) apart.
Pattern crosshatch(double spacing, double width, Color color);

// Squares @p size on a side, alternating between @p first and @p second as on a chessboard, the square from the origin
// to (size, size) in @p first. @p second fills the tile and @p first's squares are drawn over it, as a checkerboard
// is commonly drawn, so that squares meet without a seam at any scale; a translucent @p first therefore shows
// @p second through it.
Pattern checkerboard(double size, Color first, Color second);

} // namespace linea
