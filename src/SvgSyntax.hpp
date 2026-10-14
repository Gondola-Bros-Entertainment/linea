// linea - the parts of SVG's syntax that writing and reading share: numbers, whitespace and path data. Internal.
#pragma once

#include "linea/Paint.hpp"
#include "linea/Path.hpp"
#include "linea/Stroke.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace linea::detail {

// Whether @p c is white space as SVG's grammars count it: tab, line feed, form feed, carriage return or space.
constexpr bool isSvgSpace(char c) { return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' '; }

constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }

// @p text without the white space at either end.
std::string_view trimmed(std::string_view text);

// A number read from text, and where its characters end.
struct ScannedNumber {
    // The double nearest the number written, or an infinity for one too large for a double.
    double value{};
    std::size_t end{};
};

// The number that starts at @p at in @p text, written as SVG 2's grammars and CSS write numbers: an optional sign,
// digits with an optional fraction or a fraction alone, and an optional exponent. Characters are taken
// while they still form a number, so 1.5.5 starts with 1.5 and 1em with 1. Nothing when no number starts at @p at.
// The value is correctly rounded whatever the C locale's decimal point.
std::optional<ScannedNumber> scanNumber(std::string_view text, std::size_t at);

// Appends @p value to @p out in the shortest form that reads back as the same double, as std::to_chars writes it, with
// negative zero written as 0. @p value must be finite.
void appendNumber(std::string &out, double value);

// @p alpha, a byte, scaled by @p opacity, clamped to 0 to 1, and rounded to the nearest byte, as a fill-opacity,
// stroke-opacity or stop-opacity scales a color's alpha. Writing and reading both use it, so that the opacity toSvg()
// writes for a byte reads back as that byte.
std::uint8_t scaledAlpha(std::uint8_t alpha, double opacity);

// The stroke style that SVG's initial values give (SVG 2, section 13.5): width 1, miter joins, butt caps, a miter limit
// of 4 and no dashes. linea's defaults differ in the joins and caps.
StrokeStyle svgInitialStroke();

// The stops whose blend as SVG blends gradients, unpremultiplied in sRGB (SVG 2, section 14.2.4.2), draws the ramp
// that linea draws for @p stops in @p interpolation to within svgGradientTolerance: the same stops, sorted and clamped
// as linea sorts and clamps them, where the two blends agree, and more stops between them where they do not. For a
// linear gradient whose ends coincide, which linea paints the color at offset 0 and SVG the last stop's, when
// @p degenerate, the one stop of that color.
std::vector<GradientStop> svgStops(const std::vector<GradientStop> &stops, Interpolation interpolation,
                                   bool degenerate = false);

// The stops whose blend in @p space, srgb or srgbLinear, premultiplied as linea blends, draws the ramp that SVG draws
// for the sorted SVG stops @p stops, blending them unpremultiplied in the same space, to within svgGradientTolerance:
// @p stops themselves wherever neighbors share a color or an alpha, where the two blends agree, and more stops between
// those that differ in both.
std::vector<GradientStop> lineaStops(const std::vector<GradientStop> &stops, Interpolation space);

// Path data read up to its first error.
struct PathDataReading {
    Path path;
    // Where the first error lies, in characters from the start of the data, and what it is, or nothing for data without
    // one.
    std::optional<std::size_t> errorAt;
    std::string error;
};

// The path that SVG path data @p data describes, as fromSvgPathData() reads it.
PathDataReading readPathData(std::string_view data);

// A place in a text, its line and column each counted from 1.
struct TextPosition {
    int line{};
    int column{};
};

// Where the character at @p offset in the UTF-8 @p text lies. A line ends at a line feed, a carriage return, or the
// two together, and a column counts characters, not bytes.
TextPosition positionOf(std::string_view text, std::size_t offset);

} // namespace linea::detail
