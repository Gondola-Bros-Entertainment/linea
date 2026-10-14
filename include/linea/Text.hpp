// linea - text: TrueType fonts, and lines of text as paths.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace linea {

// A Font reads fonts with TrueType outlines, from their bytes and with no dependency: the head, maxp, hhea, hmtx,
// loca and glyf tables, cmap subtables in formats 4 and 12, and kern subtables in format 0.
//
// Not supported: CFF outlines, which OpenType .otf fonts keep in a CFF table instead of glyf, and kerning from the GPOS
// table, so a font that kerns only in GPOS lays out without kerning. Nor are hinting, GSUB substitutions such as
// ligatures, variable fonts, font collections, or Apple's version 1 kern table.

// A glyph's index in its font. Glyph 0 is the font's missing-character glyph, .notdef.
using GlyphId = std::uint16_t;

// What a Font throws for data that is not a TrueType font, or that is truncated or malformed. The message names the
// table at fault.
class FontError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

namespace detail {

// Characters first to last map to consecutive glyphs from glyph.
struct CharacterRange {
    char32_t first{};
    char32_t last{};
    GlyphId glyph{};
};

// A kerning pair, its left glyph in the high 16 bits of key and its right glyph in the low 16, as kern stores them.
struct KernPair {
    std::uint32_t key{};
    std::int16_t value{};
};

struct KernSubtable {
    // Sorted by key, no two the same.
    std::vector<KernPair> pairs;
    // Whether a pair's value replaces what earlier subtables add up to, instead of adding to it.
    bool replaces = false;
};

} // namespace detail

// A TrueType font's glyph outlines, metrics, character map and kerning. Every measure is in font units, unitsPerEm()
// of them to the em. Outlines are x right and y down from a glyph's origin on the baseline: TrueType's y-up
// coordinates flipped onto linea's y-down canvas.
class Font {
  public:
    // Reads the font in @p data, copying what it keeps, so @p data need not outlive the Font. Every offset, length and
    // count is checked against the data, and a font that is truncated, malformed, or in a format linea does not read
    // throws FontError. A glyph's outline is read when glyphPath() asks for it.
    explicit Font(std::span<const std::uint8_t> data);

    // The size of the em square, from 16 to 16384.
    [[nodiscard]] int unitsPerEm() const { return unitsPerEm_; }
    // How far the font's glyphs reach above the baseline, as hhea records it: positive.
    [[nodiscard]] int ascender() const { return ascender_; }
    // How far the font's glyphs reach below the baseline, as hhea records it: negative.
    [[nodiscard]] int descender() const { return descender_; }
    // The space hhea asks for between one line's descender and the next line's ascender.
    [[nodiscard]] int lineGap() const { return lineGap_; }
    // The number of glyphs, at least 1: glyphs 0 to glyphCount() - 1.
    [[nodiscard]] int glyphCount() const { return static_cast<int>(advances_.size()); }

    // The glyph that the font maps the Unicode code point @p codePoint to, or 0 for one it does not map.
    [[nodiscard]] GlyphId glyphFor(char32_t codePoint) const;
    // How far the pen moves past @p glyph, or 0 for a glyph the font does not have.
    [[nodiscard]] int advance(GlyphId glyph) const;
    // The kern table's change to the advance between @p left and the @p right that follows it, negative to set them
    // closer, or 0 for a pair it does not list.
    [[nodiscard]] int kerning(GlyphId left, GlyphId right) const;
    // The outline of @p glyph, its origin at (0, 0), as closed subpaths of lines and quadratic Bezier curves through
    // the glyph's points, a composite glyph's components moved into place. TrueType implies an on-curve point midway
    // between two consecutive off-curve points. Each contour starts at its first point if that is on the curve, else
    // at its last if that is, else midway between the two. Outer contours wind clockwise on a y-down canvas, as linea's
    // shapes do, and holes the other way; TrueType fills glyphs under the nonzero rule. Empty for a glyph without an
    // outline, such as a space, or one the font does not have. Throws FontError if the glyph's data is malformed.
    [[nodiscard]] Path glyphPath(GlyphId glyph) const;

  private:
    int unitsPerEm_{};
    int ascender_{};
    int descender_{};
    int lineGap_{};
    // One per glyph.
    std::vector<std::uint16_t> advances_;
    // Where each glyph's data starts in glyf_, in order, and one more offset where the last one's ends.
    std::vector<std::uint32_t> glyphOffsets_;
    std::vector<std::uint8_t> glyf_;
    // Sorted, and none overlap.
    std::vector<detail::CharacterRange> characters_;
    std::vector<detail::KernSubtable> kerning_;
};

// @p text, in UTF-8, as one line of @p font's glyphs at @p size units to the em, in one path. The first glyph's origin
// is at @p origin on the baseline, and each glyph after it is set its predecessor's advance further right, plus their
// kerning. A character the font does not map shows glyph 0, and each maximal ill-formed UTF-8 sequence shows the
// glyph for U+FFFD, as the Unicode Standard recommends (section 3.9). The text is one line, laid out left to right
// with no line breaking. Throws FontError if a glyph's data is malformed.
Path textPath(const Font &font, std::string_view text, double size, Vec2 origin);

// How far textPath() moves the pen across @p text at @p size: the advances of its glyphs plus their kerning.
double textAdvance(const Font &font, std::string_view text, double size);

} // namespace linea
