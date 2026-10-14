#include "support.hpp"

#include "linea/Drawing.hpp"
#include "linea/Flatten.hpp"
#include "linea/Shapes.hpp"
#include "linea/Text.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace linea;
using namespace linea::test;

namespace {

std::vector<std::uint8_t> readFile(const char *path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::filesystem::file_size(path)));
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(in);
    return bytes;
}

// Lato Regular, vendored under tests/fonts. The glyph ids, metrics and kerning below are as fontTools reads them.
const std::vector<std::uint8_t> &latoBytes() {
    static const std::vector<std::uint8_t> bytes = readFile(LINEA_TEST_FONT);
    return bytes;
}

const Font &lato() {
    static const Font font(latoBytes());
    return font;
}

constexpr GlyphId latoNotdef = 0;
constexpr GlyphId latoA = 3;
constexpr GlyphId latoI = 17;
constexpr GlyphId latoT = 36;
constexpr GlyphId latoV = 40;
constexpr GlyphId latoE = 55;
constexpr GlyphId latoO = 105;
constexpr GlyphId latoAcute = 137;
constexpr GlyphId latoColon = 170;
constexpr GlyphId latoPeriod = 171;
constexpr GlyphId latoEuro = 215;
constexpr GlyphId latoEacute = 382;

// The same verbs as @p expected, and every point within rounding of its.
void checkSamePath(const Path &actual, const Path &expected) {
    REQUIRE(actual.verbs() == expected.verbs());
    REQUIRE(actual.points().size() == expected.points().size());
    for (std::size_t i = 0; i < actual.points().size(); ++i) {
        CHECK(actual.points()[i].x == doctest::Approx(expected.points()[i].x).epsilon(1e-12));
        CHECK(actual.points()[i].y == doctest::Approx(expected.points()[i].y).epsilon(1e-12));
    }
}

int alphaAt(const Image &image, int x, int y) {
    const std::size_t pixel =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x);
    return image.rgba[pixel * 4 + 3];
}

// Fonts built in memory, with the glyphs a test needs, and small enough to corrupt byte by byte.

// Big-endian numbers, as OpenType stores them.
class Bytes {
  public:
    Bytes &u8(std::uint32_t value) {
        data_.push_back(static_cast<std::uint8_t>(value));
        return *this;
    }
    Bytes &u16(std::uint32_t value) { return u8(value >> 8).u8(value); }
    Bytes &i16(int value) { return u16(static_cast<std::uint16_t>(value)); }
    Bytes &u32(std::uint32_t value) { return u16(value >> 16).u16(value); }
    Bytes &append(const Bytes &other) {
        data_.insert(data_.end(), other.data_.begin(), other.data_.end());
        return *this;
    }
    Bytes &pad(std::size_t alignment) {
        while (data_.size() % alignment != 0)
            u8(0);
        return *this;
    }
    [[nodiscard]] std::size_t size() const { return data_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t> &data() const { return data_; }

  private:
    std::vector<std::uint8_t> data_;
};

// The sfnt version of TrueType outlines, and head's magic number (OpenType, "Organization of an OpenType font" and
// "head - Font header table").
constexpr std::uint32_t trueTypeVersion = 0x00010000;
constexpr std::uint32_t headMagic = 0x5F0F3CF5;
// glyf flags (OpenType, "glyf - Glyph data").
constexpr std::uint32_t onCurvePoint = 0x01;
constexpr std::uint32_t xShortVector = 0x02;
constexpr std::uint32_t yShortVector = 0x04;
constexpr std::uint32_t repeatFlag = 0x08;
constexpr std::uint32_t xIsSameOrPositive = 0x10;
constexpr std::uint32_t yIsSameOrPositive = 0x20;
constexpr std::uint32_t arg1And2AreWords = 0x0001;
constexpr std::uint32_t argsAreXyValues = 0x0002;
constexpr std::uint32_t weHaveAScale = 0x0008;
constexpr std::uint32_t moreComponents = 0x0020;
constexpr std::uint32_t weHaveAnXAndYScale = 0x0040;
constexpr std::uint32_t weHaveATwoByTwo = 0x0080;
constexpr std::uint32_t scaledComponentOffset = 0x0800;
// One in F2DOT14.
constexpr int f2dot14One = 16384;
// kern subtable coverage (OpenType, "kern - Kerning"), the format, 0, in the high byte.
constexpr std::uint32_t kernHorizontal = 0x0001;
constexpr std::uint32_t kernMinimum = 0x0002;
constexpr std::uint32_t kernCrossStream = 0x0004;
constexpr std::uint32_t kernOverride = 0x0008;

struct TestPoint {
    int x{};
    int y{};
    bool onCurve = true;
};

struct TestComponent {
    GlyphId glyph{};
    // argsAreXyValues for an offset, and scaledComponentOffset; the builder sets the rest.
    std::uint32_t flags{};
    // The offset, or the glyph's point and the component's point that land on each other.
    int first{};
    int second{};
    // In F2DOT14: none, a scale, x and y scales, or xscale, scale01, scale10 and yscale.
    std::vector<int> transform;
};

struct TestGlyph {
    int advance{};
    std::vector<std::vector<TestPoint>> contours;
    std::vector<TestComponent> components;
};

struct TestKern {
    GlyphId left{};
    GlyphId right{};
    int value{};
};

struct TestKernSubtable {
    std::uint32_t coverage = kernHorizontal;
    // In order of left glyph, then right.
    std::vector<TestKern> pairs;
};

struct TestFont {
    std::vector<TestGlyph> glyphs;
    // In order of character.
    std::vector<std::pair<char32_t, GlyphId>> characters;
    std::vector<TestKernSubtable> kerning;
    bool longOffsets = true;
    // 4 or 12; format 4 leaves out characters past U+FFFF.
    int cmapFormat = 12;
};

// Writes the change @p delta in one coordinate to @p out in as few bytes as hold it, and returns the flags that say
// how.
std::uint32_t coordinate(int delta, std::uint32_t shortVector, std::uint32_t sameOrPositive, Bytes &out) {
    constexpr int largestShort = 255;
    if (delta == 0)
        return sameOrPositive;
    if (std::abs(delta) <= largestShort) {
        out.u8(static_cast<std::uint32_t>(std::abs(delta)));
        return shortVector | (delta > 0 ? sameOrPositive : 0);
    }
    out.i16(delta);
    return 0;
}

Bytes simpleGlyph(const TestGlyph &glyph) {
    // numberOfContours, a bounding box linea does not read, endPtsOfContours and no instructions.
    Bytes out;
    out.i16(static_cast<int>(glyph.contours.size())).i16(0).i16(0).i16(0).i16(0);
    std::vector<TestPoint> points;
    for (const std::vector<TestPoint> &contour : glyph.contours) {
        points.insert(points.end(), contour.begin(), contour.end());
        out.u16(static_cast<std::uint32_t>(points.size() - 1));
    }
    out.u16(0);
    std::vector<std::uint32_t> flags;
    Bytes xs;
    Bytes ys;
    TestPoint last{0, 0};
    for (const TestPoint &p : points) {
        flags.push_back((p.onCurve ? onCurvePoint : 0) | coordinate(p.x - last.x, xShortVector, xIsSameOrPositive, xs) |
                        coordinate(p.y - last.y, yShortVector, yIsSameOrPositive, ys));
        last = p;
    }
    // A run of one flag is written once and repeated.
    constexpr std::size_t longestRun = 256;
    for (std::size_t i = 0; i < flags.size();) {
        std::size_t run = 1;
        while (i + run < flags.size() && flags[i + run] == flags[i] && run < longestRun)
            ++run;
        if (run == 1)
            out.u8(flags[i]);
        else
            out.u8(flags[i] | repeatFlag).u8(static_cast<std::uint32_t>(run - 1));
        i += run;
    }
    return out.append(xs).append(ys);
}

Bytes compositeGlyph(const TestGlyph &glyph) {
    Bytes out;
    out.i16(-1).i16(0).i16(0).i16(0).i16(0);
    for (std::size_t i = 0; i < glyph.components.size(); ++i) {
        const TestComponent &c = glyph.components[i];
        std::uint32_t flags = c.flags | arg1And2AreWords;
        if (i + 1 < glyph.components.size())
            flags |= moreComponents;
        if (c.transform.size() == 1)
            flags |= weHaveAScale;
        else if (c.transform.size() == 2)
            flags |= weHaveAnXAndYScale;
        else if (c.transform.size() == 4)
            flags |= weHaveATwoByTwo;
        out.u16(flags).u16(c.glyph).i16(c.first).i16(c.second);
        for (const int value : c.transform)
            out.i16(value);
    }
    return out;
}

Bytes characterMap(const TestFont &font) {
    Bytes table;
    Bytes subtable;
    if (font.cmapFormat == 12) {
        // One encoding record, Windows' full Unicode repertoire, then one group a character.
        table.u16(0).u16(1).u16(3).u16(10).u32(12);
        subtable.u16(12).u16(0).u32(static_cast<std::uint32_t>(16 + 12 * font.characters.size())).u32(0);
        subtable.u32(static_cast<std::uint32_t>(font.characters.size()));
        for (const auto &[c, glyph] : font.characters)
            subtable.u32(c).u32(c).u32(glyph);
        return table.append(subtable);
    }
    // One encoding record, Windows' Basic Multilingual Plane, then one segment a character, alternately through
    // idDelta and through glyphIdArray, and the final segment at U+FFFF.
    std::vector<std::pair<char32_t, GlyphId>> bmp;
    for (const auto &mapping : font.characters)
        if (mapping.first <= 0xFFFF)
            bmp.push_back(mapping);
    const std::size_t segments = bmp.size() + 1;
    Bytes ends;
    Bytes starts;
    Bytes deltas;
    Bytes rangeOffsets;
    Bytes glyphIds;
    for (std::size_t i = 0; i < bmp.size(); ++i) {
        const auto [c, glyph] = bmp[i];
        ends.u16(c);
        starts.u16(c);
        if (i % 2 == 0) {
            deltas.u16(static_cast<std::uint16_t>(char32_t{glyph} - c));
            rangeOffsets.u16(0);
        } else {
            deltas.u16(0);
            rangeOffsets.u16(static_cast<std::uint32_t>(2 * (segments - i) + glyphIds.size()));
            glyphIds.u16(glyph);
        }
    }
    ends.u16(0xFFFF);
    starts.u16(0xFFFF);
    deltas.u16(1);
    rangeOffsets.u16(0);
    table.u16(0).u16(1).u16(3).u16(1).u32(12);
    const auto length = static_cast<std::uint32_t>(16 + 8 * segments + glyphIds.size());
    subtable.u16(4).u16(length).u16(0).u16(static_cast<std::uint32_t>(2 * segments)).u16(0).u16(0).u16(0);
    subtable.append(ends).u16(0).append(starts).append(deltas).append(rangeOffsets).append(glyphIds);
    return table.append(subtable);
}

std::vector<std::uint8_t> build(const TestFont &font) {
    constexpr std::uint32_t unitsPerEm = 1000;
    Bytes glyf;
    Bytes loca;
    Bytes hmtx;
    const auto locate = [&] {
        if (font.longOffsets)
            loca.u32(static_cast<std::uint32_t>(glyf.size()));
        else
            loca.u16(static_cast<std::uint32_t>(glyf.size() / 2));
    };
    for (const TestGlyph &glyph : font.glyphs) {
        locate();
        if (!glyph.components.empty())
            glyf.append(compositeGlyph(glyph));
        else if (!glyph.contours.empty())
            glyf.append(simpleGlyph(glyph));
        glyf.pad(2);
        hmtx.u16(static_cast<std::uint32_t>(glyph.advance)).i16(0);
    }
    locate();
    const auto glyphCount = static_cast<std::uint32_t>(font.glyphs.size());

    // version, fontRevision, checksumAdjustment, magicNumber, flags, unitsPerEm, created and modified; then the
    // bounding box, macStyle, lowestRecPPEM, fontDirectionHint, indexToLocFormat and glyphDataFormat.
    Bytes head;
    head.u32(trueTypeVersion).u32(0).u32(0).u32(headMagic).u16(0).u16(unitsPerEm).u32(0).u32(0).u32(0).u32(0);
    head.i16(0).i16(0).i16(0).i16(0).u16(0).u16(0).i16(0).i16(font.longOffsets ? 1 : 0).i16(0);
    // version, ascender, descender, lineGap and advanceWidthMax, then 11 fields linea does not read, then
    // numberOfHMetrics.
    Bytes hhea;
    hhea.u32(trueTypeVersion).i16(800).i16(-200).i16(90).u16(0);
    for (int field = 0; field < 11; ++field)
        hhea.i16(0);
    hhea.u16(glyphCount);
    // Version 1.0: numGlyphs, then 13 fields linea does not read.
    Bytes maxp;
    maxp.u32(trueTypeVersion).u16(glyphCount);
    for (int field = 0; field < 13; ++field)
        maxp.u16(0);
    // Version 0, then format 0 subtables: version, length and coverage, then nPairs, three fields linea does not
    // read, and the pairs.
    Bytes kern;
    kern.u16(0).u16(static_cast<std::uint32_t>(font.kerning.size()));
    for (const TestKernSubtable &subtable : font.kerning) {
        kern.u16(0).u16(static_cast<std::uint32_t>(14 + 6 * subtable.pairs.size())).u16(subtable.coverage);
        kern.u16(static_cast<std::uint32_t>(subtable.pairs.size())).u16(0).u16(0).u16(0);
        for (const TestKern &pair : subtable.pairs)
            kern.u16(pair.left).u16(pair.right).i16(pair.value);
    }
    const Bytes cmap = characterMap(font);

    // The tables in order of tag, as the directory lists them, each but the last padded to a 4-byte boundary.
    const std::pair<std::string_view, const Bytes *> tables[] = {
        {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea},
        {"hmtx", &hmtx}, {"kern", &kern}, {"loca", &loca}, {"maxp", &maxp},
    };
    const std::size_t tableCount = std::size(tables);
    const std::size_t directorySize = 12 + 16 * tableCount;
    Bytes directory;
    directory.u32(trueTypeVersion).u16(static_cast<std::uint32_t>(tableCount)).u16(0).u16(0).u16(0);
    Bytes body;
    for (std::size_t i = 0; i < tableCount; ++i) {
        const auto [tag, table] = tables[i];
        for (const char c : tag)
            directory.u8(static_cast<std::uint8_t>(c));
        directory.u32(0).u32(static_cast<std::uint32_t>(directorySize + body.size()));
        directory.u32(static_cast<std::uint32_t>(table->size()));
        body.append(*table);
        if (i + 1 < tableCount)
            body.pad(4);
    }
    return directory.append(body).data();
}

// The test font's glyphs, each for one part of glyf.
constexpr GlyphId square = 0;
constexpr GlyphId startsOffCurve = 1;
constexpr GlyphId allOffCurve = 2;
constexpr GlyphId turned = 3;
constexpr GlyphId scaledOffset = 4;
constexpr GlyphId stretched = 5;
constexpr GlyphId matched = 6;
constexpr GlyphId nested = 7;
constexpr GlyphId cycle = 8;
constexpr GlyphId missingComponent = 9;
constexpr GlyphId space = 10;
constexpr GlyphId unmatched = 11;

TestFont testFont() {
    TestFont font;
    font.glyphs = {
        {600, {{{0, 0}, {0, 700}, {500, 700}, {500, 0}}}, {}},
        {300, {{{100, 0, false}, {200, 100}, {100, 200, false}, {0, 100}}}, {}},
        {300, {{{100, 0, false}, {200, 100, false}, {100, 200, false}, {0, 100, false}}}, {}},
        // A quarter turn, x' = -y and y' = x, then 1000 to the right, which the turn does not move.
        {600, {}, {{square, argsAreXyValues, 1000, 0, {0, f2dot14One, -f2dot14One, 0}}}},
        // Half size, its offset halved with it.
        {600, {}, {{square, argsAreXyValues | scaledComponentOffset, 100, 200, {f2dot14One / 2}}}},
        // Half again as wide and half as tall, its offset as it is.
        {600, {}, {{square, argsAreXyValues, 100, 200, {f2dot14One * 3 / 2, f2dot14One / 2}}}},
        // A second square, its point 0 on the first square's point 2.
        {600, {}, {{square, argsAreXyValues, 0, 0, {}}, {square, 0, 2, 0, {}}}},
        {600, {}, {{turned, argsAreXyValues, 0, 100, {}}}},
        {600, {}, {{cycle, argsAreXyValues, 0, 0, {}}}},
        {600, {}, {{200, argsAreXyValues, 0, 0, {}}}},
        {250, {}, {}},
        // A second square on the first square's point 4, which it does not have.
        {600, {}, {{square, argsAreXyValues, 0, 0, {}}, {square, 0, 4, 0, {}}}},
    };
    font.characters = {{U' ', space},  {U'A', startsOffCurve}, {U'B', allOffCurve},
                       {U'C', turned}, {U'D', matched},        {U'\U0001F600', scaledOffset}};
    font.kerning = {{kernHorizontal, {{startsOffCurve, allOffCurve, -50}, {allOffCurve, startsOffCurve, 30}}}};
    return font;
}

TestFont shortFormat4(TestFont font) {
    font.longOffsets = false;
    font.cmapFormat = 4;
    return font;
}

std::size_t get16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::size_t{bytes[offset]} << 8 | std::size_t{bytes[offset + 1]};
}

void put16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

// Where the table directory's record for @p tag starts in @p font.
std::size_t recordOf(std::span<const std::uint8_t> font, std::string_view tag) {
    const std::size_t end = 12 + 16 * get16(font, 4);
    std::size_t record = 12;
    while (record < end && std::string_view(reinterpret_cast<const char *>(&font[record]), 4) != tag)
        record += 16;
    REQUIRE(record < end);
    return record;
}

// Where the table tagged @p tag starts in @p font.
std::size_t tableOf(std::span<const std::uint8_t> font, std::string_view tag) {
    const std::size_t offset = recordOf(font, tag) + 8;
    return std::size_t{font[offset]} << 24 | std::size_t{font[offset + 1]} << 16 | std::size_t{font[offset + 2]} << 8 |
           std::size_t{font[offset + 3]};
}

// Reads everything in @p bytes that a program drawing text would, and whether what any font that reads promises
// holds: every character maps to a glyph the font has, and every outline is finite. Malformed data throws FontError,
// which this expects; anything else fails, an out-of-bounds read or undefined behavior that the sanitizers catch
// included.
bool readsSafely(std::span<const std::uint8_t> bytes) {
    try {
        const Font font(bytes);
        bool safe = true;
        for (const char32_t c : {U' ', U'A', U'B', U'C', U'D', U'\U0001F600', U'\uFFFF'})
            safe = safe && font.glyphFor(c) < font.glyphCount();
        for (int glyph = 0; glyph < font.glyphCount(); ++glyph) {
            try {
                const Path outline = font.glyphPath(static_cast<GlyphId>(glyph));
                for (const Vec2 p : outline.points())
                    safe = safe && std::isfinite(p.x) && std::isfinite(p.y);
            } catch (const FontError &) {
            }
        }
        try {
            static_cast<void>(textPath(font, "AB CD \xF0\x9F\x98\x80", 24.0, {}));
        } catch (const FontError &) {
        }
        return safe;
    } catch (const FontError &) {
        return true;
    }
}

} // namespace

TEST_CASE("A font reads the em size, line metrics and glyph count that head, hhea and maxp record") {
    CHECK(lato().unitsPerEm() == 2000);
    CHECK(lato().ascender() == 1974);
    CHECK(lato().descender() == -426);
    CHECK(lato().lineGap() == 0);
    CHECK(lato().glyphCount() == 467);
}

TEST_CASE("cmap format 4 maps characters through idDelta and glyphIdArray, and unmapped ones to glyph 0") {
    // A and e-acute lie in segments read through glyphIdArray.
    CHECK(lato().glyphFor(U'A') == latoA);
    CHECK(lato().glyphFor(U'\u00E9') == latoEacute);
    // The euro sign's segment adds an idDelta that wraps past 65535: 0x20AC + 57387 is 215 modulo 65536.
    CHECK(lato().glyphFor(U'\u20AC') == latoEuro);
    CHECK(lato().glyphFor(U'\u4E00') == 0);
    CHECK(lato().glyphFor(U'\uFFFF') == 0);
    CHECK(lato().glyphFor(U'\U0001F600') == 0);
    CHECK(lato().glyphFor(char32_t{0x110000}) == 0);
}

TEST_CASE("cmap format 12 maps characters beyond the Basic Multilingual Plane") {
    const std::vector<std::uint8_t> bytes = build(testFont());
    const Font font(bytes);
    CHECK(font.glyphFor(U'\U0001F600') == scaledOffset);
    CHECK(font.glyphFor(U'\U0001F601') == 0);
    CHECK(font.glyphFor(U'A') == startsOffCurve);
    CHECK(font.glyphFor(U'@') == 0);
}

TEST_CASE("A glyph's path runs through its outline points in font units, y flipped, with implied on-curve midpoints") {
    // What fontTools draws for Lato's o, its quadratic splines split into curves at the midpoints between consecutive
    // off-curve points.
    Path o;
    o.moveTo({568, -1029})
        .quadTo({679, -1029}, {768, -992})
        .quadTo({857, -955}, {920, -887})
        .quadTo({983, -819}, {1016.5, -722.5})
        .quadTo({1050, -626}, {1050, -507})
        .quadTo({1050, -387}, {1016.5, -291})
        .quadTo({983, -195}, {920, -127})
        .quadTo({857, -59}, {768, -22.5})
        .quadTo({679, 14}, {568, 14})
        .quadTo({456, 14}, {366.5, -22.5})
        .quadTo({277, -59}, {214, -127})
        .quadTo({151, -195}, {117.5, -291})
        .quadTo({84, -387}, {84, -507})
        .quadTo({84, -626}, {117.5, -722.5})
        .quadTo({151, -819}, {214, -887})
        .quadTo({277, -955}, {366.5, -992})
        .quadTo({456, -1029}, {568, -1029})
        .close()
        .moveTo({568, -125})
        .quadTo({718, -125}, {792, -225.5})
        .quadTo({866, -326}, {866, -506})
        .quadTo({866, -687}, {792, -788})
        .quadTo({718, -889}, {568, -889})
        .quadTo({492, -889}, {435.5, -863})
        .quadTo({379, -837}, {341.5, -788})
        .quadTo({304, -739}, {285.5, -667.5})
        .quadTo({267, -596}, {267, -506})
        .quadTo({267, -326}, {341.5, -225.5})
        .quadTo({416, -125}, {568, -125})
        .close();
    CHECK(lato().glyphPath(latoO) == o);
    Path i;
    i.moveTo({377, 0}).lineTo({183, 0}).lineTo({183, -1433}).lineTo({377, -1433}).close();
    CHECK(lato().glyphPath(latoI) == i);
    CHECK(lato().glyphPath(lato().glyphFor(U' ')).empty());
    CHECK(lato().glyphPath(static_cast<GlyphId>(lato().glyphCount())).empty());
}

TEST_CASE("Glyph outlines wind clockwise on a y-down canvas, as shapes do, and their holes the other way") {
    const std::vector<Contour> contours = flatten(lato().glyphPath(latoO), 0.1);
    REQUIRE(contours.size() == 2);
    CHECK(twiceSignedArea(contours[0]) > 0.0);
    CHECK(twiceSignedArea(contours[1]) < 0.0);
}

TEST_CASE("A contour that starts off the curve starts at its last point, or midway between its first and last") {
    const std::vector<std::uint8_t> bytes = build(testFont());
    const Font font(bytes);
    Path fromLast;
    fromLast.moveTo({0, -100}).quadTo({100, 0}, {200, -100}).quadTo({100, -200}, {0, -100}).close();
    CHECK(font.glyphPath(startsOffCurve) == fromLast);
    Path fromMidpoint;
    fromMidpoint.moveTo({50, -50})
        .quadTo({100, 0}, {150, -50})
        .quadTo({200, -100}, {150, -150})
        .quadTo({100, -200}, {50, -150})
        .quadTo({0, -100}, {50, -50})
        .close();
    CHECK(font.glyphPath(allOffCurve) == fromMidpoint);
}

TEST_CASE("A composite glyph resolves to its components, each moved by its offset") {
    Path eacute = lato().glyphPath(latoE);
    eacute.append(transformed(lato().glyphPath(latoAcute), translate({160, 0})));
    CHECK(lato().glyphPath(latoEacute) == eacute);
    Path colon = transformed(lato().glyphPath(latoPeriod), translate({14, 0}));
    colon.append(transformed(lato().glyphPath(latoPeriod), translate({14, -750})));
    CHECK(lato().glyphPath(latoColon) == colon);
}

TEST_CASE("Component transforms, scaled offsets and matched points place components as glyf specifies") {
    const std::vector<std::uint8_t> bytes = build(testFont());
    const Font font(bytes);
    const auto box = [](Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
        Path path;
        path.moveTo(a).lineTo(b).lineTo(c).lineTo(d).close();
        return path;
    };
    // The square's corners, in font units with y up, are (0, 0), (0, 700), (500, 700) and (500, 0).
    CHECK(font.glyphPath(square) == box({0, 0}, {0, -700}, {500, -700}, {500, 0}));
    checkSamePath(font.glyphPath(turned), box({1000, 0}, {300, 0}, {300, -500}, {1000, -500}));
    checkSamePath(font.glyphPath(scaledOffset), box({50, -100}, {50, -450}, {300, -450}, {300, -100}));
    checkSamePath(font.glyphPath(stretched), box({100, -200}, {100, -550}, {850, -550}, {850, -200}));
    Path pair = font.glyphPath(square);
    pair.append(transformed(font.glyphPath(square), translate({500, -700})));
    CHECK(font.glyphPath(matched) == pair);
    checkSamePath(font.glyphPath(nested), transformed(font.glyphPath(turned), translate({0, -100})));
}

TEST_CASE("Short and long loca offsets, and cmap formats 4 and 12, read the same glyphs") {
    const std::vector<std::uint8_t> longBytes = build(testFont());
    const std::vector<std::uint8_t> shortBytes = build(shortFormat4(testFont()));
    const Font longForm(longBytes);
    const Font shortForm(shortBytes);
    for (const GlyphId glyph :
         {square, startsOffCurve, allOffCurve, turned, scaledOffset, stretched, matched, nested, space}) {
        CHECK(shortForm.glyphPath(glyph) == longForm.glyphPath(glyph));
        CHECK(shortForm.advance(glyph) == longForm.advance(glyph));
    }
    for (const char32_t c : {U' ', U'A', U'B', U'C', U'D', U'E'})
        CHECK(shortForm.glyphFor(c) == longForm.glyphFor(c));
    CHECK(shortForm.glyphFor(U'\U0001F600') == 0);
}

TEST_CASE("Advances and kerning place each glyph where hmtx and kern say") {
    CHECK(lato().advance(latoV) == 1354);
    CHECK(lato().advance(latoA) == 1354);
    CHECK(lato().kerning(latoV, latoA) == -108);
    CHECK(lato().kerning(latoT, latoO) == -244);
    // Lato kerns A then V only in GPOS, which linea does not read.
    CHECK(lato().kerning(latoA, latoV) == 0);
    CHECK(lato().advance(static_cast<GlyphId>(lato().glyphCount())) == 0);

    constexpr double size = 100.0;
    constexpr double perUnit = size / 2000.0;
    const Vec2 origin{10.0, 50.0};
    Path expected = transformed(lato().glyphPath(latoV), translate(origin) * scale(perUnit));
    expected.append(transformed(lato().glyphPath(latoA),
                                translate({origin.x + (1354 - 108) * perUnit, origin.y}) * scale(perUnit)));
    checkSamePath(textPath(lato(), "VA", size, origin), expected);
    CHECK(textAdvance(lato(), "VA", size) == doctest::Approx((1354 - 108 + 1354) * perUnit));
    CHECK(textAdvance(lato(), "To", size) == doctest::Approx((1181 - 244 + 1134) * perUnit));
    CHECK(textAdvance(lato(), "", size) == 0.0);
}

TEST_CASE(
    "Kerning adds up across kern subtables until one overrides it, and minimum and cross-stream values do not kern") {
    TestFont font = testFont();
    const auto subtable = [](std::uint32_t coverage, int value) {
        return TestKernSubtable{coverage, {{startsOffCurve, allOffCurve, value}}};
    };
    font.kerning = {subtable(kernHorizontal, -50), subtable(kernHorizontal, -20),
                    subtable(kernHorizontal | kernMinimum, -400), subtable(kernHorizontal | kernCrossStream, -400)};
    const std::vector<std::uint8_t> added = build(font);
    CHECK(Font(added).kerning(startsOffCurve, allOffCurve) == -70);
    font.kerning.push_back(subtable(kernHorizontal | kernOverride, 15));
    const std::vector<std::uint8_t> overridden = build(font);
    CHECK(Font(overridden).kerning(startsOffCurve, allOffCurve) == 15);
    CHECK(Font(overridden).kerning(allOffCurve, startsOffCurve) == 0);
}

TEST_CASE("Kerning pairs out of order still kern, and of two for the same glyphs the first counts") {
    TestFont font = testFont();
    font.kerning = {
        {kernHorizontal,
         {{turned, square, -30}, {startsOffCurve, allOffCurve, -50}, {turned, square, 99}, {square, turned, 10}}}};
    const std::vector<std::uint8_t> unsorted = build(font);
    const Font read(unsorted);
    CHECK(read.kerning(square, turned) == 10);
    CHECK(read.kerning(startsOffCurve, allOffCurve) == -50);
    CHECK(read.kerning(turned, square) == -30);
    CHECK(read.kerning(allOffCurve, startsOffCurve) == 0);
}

TEST_CASE("UTF-8 decodes to code points, and each maximal ill-formed sequence to U+FFFD") {
    // At 2000 units to the em, Lato's own, advances are in font units. Lato has no U+FFFD, so each ill-formed sequence
    // shows .notdef, 1063 units wide, which kerns with nothing.
    const auto advance = [](std::string_view text) { return textAdvance(lato(), text, 2000.0); };
    CHECK(lato().advance(latoNotdef) == 1063);
    CHECK(advance("\xC3\xA9") == 1056.0);
    CHECK(advance("\xE2\x82\xAC") == 1160.0);
    // A sequence cut short is one ill-formed sequence.
    CHECK(advance("\xE2\x82"
                  "A") == 1063.0 + 1354.0);
    CHECK(advance("\xF0\x9F\x98") == 1063.0);
    // C0 never starts a sequence, so it and the byte after it are one each.
    CHECK(advance("\xC0\xAF") == 2 * 1063.0);
    // An encoded surrogate, or a code point past U+10FFFF, is ill-formed from its second byte.
    CHECK(advance("\xED\xA0\x80") == 3 * 1063.0);
    CHECK(advance("\xF4\x90\x80\x80") == 4 * 1063.0);
}

TEST_CASE("Text renders with the same exact-area anti-aliasing as shapes, at any size") {
    // Lato's I is the rectangle from (183, 0) to (377, 1433). At 125 units to the em, a sixteenth of the font's 2000,
    // every coordinate is exact in binary, so the text and the shape rasterize the very same edges.
    constexpr double size = 125.0;
    constexpr double perUnit = size / 2000.0;
    const Vec2 origin{2.0, 95.0};
    Layer text;
    text.path = textPath(lato(), "I", size, origin);
    text.fill = Paint{Color{20, 20, 20, 255}};
    Layer shape = text;
    shape.path = rectangle({origin.x + 183 * perUnit, origin.y - 1433 * perUnit}, {origin.x + 377 * perUnit, origin.y});
    for (const int zoom : {1, 2, 3})
        CHECK(render({40.0, 100.0, {text}}, 40 * zoom, 100 * zoom).rgba ==
              render({40.0, 100.0, {shape}}, 40 * zoom, 100 * zoom).rgba);
    // The stem's left edge, at x 13.4375, covers 0.5625 of each pixel it crosses.
    const Image drawn = render({40.0, 100.0, {text}}, 40, 100);
    CHECK(alphaAt(drawn, 12, 50) == 0);
    CHECK(alphaAt(drawn, 13, 50) == 143);
    CHECK(alphaAt(drawn, 14, 50) == 255);
}

TEST_CASE("A glyph covers exactly the area inside its outline, its hole cut out") {
    const std::vector<Contour> contours = flatten(textPath(lato(), "o", 200.0, {0.0, 110.0}), 0.01);
    REQUIRE(contours.size() == 2);
    const double area = 0.5 * (twiceSignedArea(contours[0]) + twiceSignedArea(contours[1]));
    const std::vector<float> cover = coverage(contours, 120, 120, FillRule::nonZero);
    CHECK(total(cover) == doctest::Approx(area).epsilon(1e-9));
    // The middle of the hole is empty, and the middle of the ring's left side full.
    CHECK(at(cover, 120, 56, 59) == doctest::Approx(0.0));
    CHECK(at(cover, 120, 17, 59) == doctest::Approx(1.0));
}

TEST_CASE("Truncated fonts are rejected") {
    const std::vector<std::uint8_t> bytes = build(testFont());
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK_THROWS_AS(Font{std::span(bytes).first(length)}, FontError);
    const std::span<const std::uint8_t> whole(latoBytes());
    for (const std::size_t length : {std::size_t{0}, std::size_t{11}, std::size_t{283}, whole.size() - 1})
        CHECK_THROWS_AS(Font{whole.first(length)}, FontError);
}

TEST_CASE("Fonts that are not TrueType, or whose tables contradict each other, are rejected naming the table") {
    const auto changed = [](std::string_view tag, std::size_t offset, std::uint32_t value) {
        std::vector<std::uint8_t> bytes = latoBytes();
        put16(bytes, tableOf(bytes, tag) + offset, value);
        return bytes;
    };
    std::vector<std::uint8_t> cff = latoBytes();
    put16(cff, 0, 0x4F54);
    put16(cff, 2, 0x544F);
    CHECK_THROWS_WITH_AS(Font{cff}, doctest::Contains("CFF outlines are not supported"), FontError);
    std::vector<std::uint8_t> collection = latoBytes();
    put16(collection, 0, 0x7474);
    put16(collection, 2, 0x6366);
    CHECK_THROWS_WITH_AS(Font{collection}, doctest::Contains("collections are not supported"), FontError);

    // head's magicNumber, unitsPerEm and indexToLocFormat.
    CHECK_THROWS_WITH_AS(Font{changed("head", 12, 0)}, doctest::Contains("head:"), FontError);
    CHECK_THROWS_WITH_AS(Font{changed("head", 18, 0)}, doctest::Contains("head:"), FontError);
    CHECK_THROWS_WITH_AS(Font{changed("head", 50, 2)}, doctest::Contains("head:"), FontError);
    // More glyphs than loca has offsets for, and no glyphs at all.
    CHECK_THROWS_WITH_AS(Font{changed("maxp", 4, 0xFFFF)}, doctest::Contains("loca:"), FontError);
    CHECK_THROWS_WITH_AS(Font{changed("maxp", 4, 0)}, doctest::Contains("maxp:"), FontError);
    // No advances, and more advances than glyphs.
    CHECK_THROWS_WITH_AS(Font{changed("hhea", 34, 0)}, doctest::Contains("hhea:"), FontError);
    CHECK_THROWS_WITH_AS(Font{changed("hhea", 34, 468)}, doctest::Contains("hhea:"), FontError);
    // A glyph offset past the end of glyf, and one before the glyph before it.
    CHECK_THROWS_WITH_AS(Font{changed("loca", 2, 0xFFFF)}, doctest::Contains("loca:"), FontError);
    CHECK_THROWS_WITH_AS(Font{changed("loca", 4, 0)}, doctest::Contains("loca:"), FontError);
    // A table that reaches past the end of the data, and a missing table.
    std::vector<std::uint8_t> overlong = latoBytes();
    put16(overlong, recordOf(overlong, "name") + 12, 0x7FFF);
    CHECK_THROWS_WITH_AS(Font{overlong}, doctest::Contains("table directory:"), FontError);
    std::vector<std::uint8_t> missing = latoBytes();
    missing[recordOf(missing, "cmap") + 3] = 'q';
    CHECK_THROWS_WITH_AS(Font{missing}, doctest::Contains("cmap"), FontError);
}

TEST_CASE("A simple glyph whose contours end out of order, or whose flags repeat past its last point, is rejected") {
    // Lato's o has two contours, which end at points 19 and 33; the second now ends at 19 too.
    std::vector<std::uint8_t> reordered = latoBytes();
    const std::size_t o = tableOf(reordered, "glyf") + 2 * get16(reordered, tableOf(reordered, "loca") + 2 * latoO);
    REQUIRE(get16(reordered, o + 12) == 33);
    put16(reordered, o + 12, 19);
    CHECK_THROWS_WITH_AS(static_cast<void>(Font(reordered).glyphPath(latoO)), doctest::Contains("glyf:"), FontError);
    // The test square's first flag, at byte 14 of its glyph, now repeats as many times as the next byte, 0x11, says.
    std::vector<std::uint8_t> bytes = build(testFont());
    const std::size_t flags = tableOf(bytes, "glyf") + 14;
    REQUIRE(bytes[flags + 1] == 0x11);
    bytes[flags] = static_cast<std::uint8_t>(bytes[flags] | repeatFlag);
    CHECK_THROWS_WITH_AS(static_cast<void>(Font(bytes).glyphPath(square)), doctest::Contains("glyf:"), FontError);
}

TEST_CASE("Composite cycles, missing components, unmatched points and runaway nesting are rejected") {
    const std::vector<std::uint8_t> bytes = build(testFont());
    const Font font(bytes);
    CHECK_THROWS_WITH_AS(static_cast<void>(font.glyphPath(cycle)), doctest::Contains("glyf:"), FontError);
    CHECK_THROWS_WITH_AS(static_cast<void>(font.glyphPath(missingComponent)), doctest::Contains("glyf:"), FontError);
    CHECK_THROWS_WITH_AS(static_cast<void>(font.glyphPath(unmatched)), doctest::Contains("glyf:"), FontError);
    // A glyph of 256 components, each of 256 empty ones, reaches past the 65535 components TrueType can count.
    TestFont fanOut;
    fanOut.glyphs = {{0, {}, {}}, {0, {}, {}}, {0, {}, {}}};
    fanOut.glyphs[1].components.assign(256, {2, argsAreXyValues, 0, 0, {}});
    fanOut.glyphs[2].components.assign(256, {0, argsAreXyValues, 0, 0, {}});
    const std::vector<std::uint8_t> fanOutBytes = build(fanOut);
    const Font fanned(fanOutBytes);
    CHECK(fanned.glyphPath(2).empty());
    CHECK_THROWS_WITH_AS(static_cast<void>(fanned.glyphPath(1)), doctest::Contains("glyf:"), FontError);
}

TEST_CASE("Every single-byte corruption of a font is read or rejected, never read out of bounds" *
          doctest::test_suite("slow")) {
    for (const TestFont &font : {testFont(), shortFormat4(testFont())}) {
        const std::vector<std::uint8_t> clean = build(font);
        REQUIRE(readsSafely(clean));
        for (std::size_t offset = 0; offset < clean.size(); ++offset) {
            const std::uint32_t original = clean[offset];
            for (const std::uint32_t value : {0x00u, 0xFFu, original ^ 0x01u, original ^ 0x80u}) {
                std::vector<std::uint8_t> corrupted = clean;
                corrupted[offset] = static_cast<std::uint8_t>(value);
                CAPTURE(offset);
                CAPTURE(value);
                CHECK(readsSafely(corrupted));
            }
        }
    }
}
