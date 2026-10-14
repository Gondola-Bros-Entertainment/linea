// linea - Text implementation: a TrueType reader after the OpenType specification, whose chapter on each table the
// constants below cite, and line layout. Every read goes through a Reader that checks it against the table it reads.
#include "linea/Text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace linea {
namespace {

// A tag's four ASCII characters as one big-endian number, as the table directory stores it (OpenType, "Data types").
constexpr std::uint32_t tagOf(std::string_view name) {
    std::uint32_t tag = 0;
    for (const char c : name)
        tag = tag << 8 | static_cast<std::uint8_t>(c);
    return tag;
}

// The sizes of OpenType's 16-bit and 32-bit numbers, in bytes.
constexpr std::size_t size16 = 2;
constexpr std::size_t size32 = 4;

// The table directory (OpenType, "Organization of an OpenType font"). A font starts with its sfnt version: 0x00010000
// for TrueType outlines, which Apple's TrueType Reference Manual also writes 'true', or 'OTTO' for CFF outlines. A
// font collection starts with 'ttcf' instead (OpenType, "Font collections").
constexpr std::uint32_t trueTypeVersion = 0x00010000;
constexpr std::uint32_t appleTrueTypeVersion = tagOf("true");
constexpr std::uint32_t cffVersion = tagOf("OTTO");
constexpr std::uint32_t collectionTag = tagOf("ttcf");
// numTables at byte 4, then from byte 12 a 16-byte record per table: tag, checksum, offset and length.
constexpr std::size_t directoryTableCount = 4;
constexpr std::size_t directoryRecords = 12;
constexpr std::size_t directoryRecordSize = 16;
constexpr std::size_t recordOffset = 8;
constexpr std::size_t recordLength = 12;

// head (OpenType, "head - Font header table"): magicNumber, unitsPerEm and indexToLocFormat.
constexpr std::size_t headMagicNumber = 12;
constexpr std::uint32_t headMagic = 0x5F0F3CF5;
constexpr std::size_t headUnitsPerEm = 18;
constexpr int fewestUnitsPerEm = 16;
constexpr int mostUnitsPerEm = 16384;
constexpr std::size_t headIndexToLocFormat = 50;
// loca holds 16-bit offsets, each half the true offset, or 32-bit offsets.
constexpr std::int16_t shortOffsets = 0;
constexpr std::int16_t longOffsets = 1;
constexpr std::uint32_t shortOffsetScale = 2;

// maxp (OpenType, "maxp - Maximum profile"): numGlyphs.
constexpr std::size_t maxpNumGlyphs = 4;

// hhea (OpenType, "hhea - Horizontal header table"): ascender, descender, lineGap and numberOfHMetrics.
constexpr std::size_t hheaAscender = 4;
constexpr std::size_t hheaDescender = 6;
constexpr std::size_t hheaLineGap = 8;
constexpr std::size_t hheaNumberOfHMetrics = 34;

// hmtx (OpenType, "hmtx - Horizontal metrics table"): numberOfHMetrics 4-byte records that start with advanceWidth.
// Glyphs past the last record take its advance.
constexpr std::size_t longHorMetricSize = 4;

// glyf (OpenType, "glyf - Glyph data"): a 10-byte header that starts with numberOfContours, negative for a composite
// glyph.
constexpr std::size_t glyphHeaderSize = 10;
// A simple glyph's flags.
constexpr std::uint8_t onCurvePoint = 0x01;
constexpr std::uint8_t xShortVector = 0x02;
constexpr std::uint8_t yShortVector = 0x04;
constexpr std::uint8_t repeatFlag = 0x08;
constexpr std::uint8_t xIsSameOrPositive = 0x10;
constexpr std::uint8_t yIsSameOrPositive = 0x20;
// A composite glyph's component flags.
constexpr std::uint16_t arg1And2AreWords = 0x0001;
constexpr std::uint16_t argsAreXyValues = 0x0002;
constexpr std::uint16_t weHaveAScale = 0x0008;
constexpr std::uint16_t moreComponents = 0x0020;
constexpr std::uint16_t weHaveAnXAndYScale = 0x0040;
constexpr std::uint16_t weHaveATwoByTwo = 0x0080;
constexpr std::uint16_t scaledComponentOffset = 0x0800;
constexpr std::uint16_t unscaledComponentOffset = 0x1000;
// One in F2DOT14, a signed fixed-point number with 14 fraction bits (OpenType, "Data types").
constexpr double f2dot14One = 16384.0;
// TrueType numbers a glyph's points with 16-bit indices (endPtsOfContours, and the points that composite glyphs
// match), so no glyph has more points than this.
constexpr std::size_t mostGlyphPoints = std::size_t{1} << 16;
// maxp counts a glyph's components in 16-bit fields (OpenType, "maxp - Maximum profile": maxComponentElements), so a
// glyph that reaches more than this through nested composites is malformed. The bound also stops a font from nesting
// composites into exponential work.
constexpr std::size_t mostGlyphComponents = std::numeric_limits<std::uint16_t>::max();
// Fonts nest components a level or two deep, an accent on a composed letter. Nesting deeper than this is a cycle or an
// attack, and the bound keeps the recursion that resolves components shallow.
constexpr int deepestComponentNesting = 64;

// cmap (OpenType, "cmap - Character to glyph index mapping table"): numTables at byte 2, then from byte 4 an 8-byte
// encoding record per subtable: platformID, encodingID and the subtable's offset.
constexpr std::size_t cmapTableCount = 2;
constexpr std::size_t cmapRecords = 4;
constexpr std::size_t cmapRecordSize = 8;
constexpr std::size_t cmapRecordEncoding = 2;
constexpr std::size_t cmapRecordOffset = 4;
// The encodings that map Unicode: platform 0 in every encoding, and on platform 3, Windows, encodings 1 for the Basic
// Multilingual Plane and 10 for the full repertoire.
constexpr std::uint16_t unicodePlatform = 0;
constexpr std::uint16_t windowsPlatform = 3;
constexpr std::uint16_t windowsUnicodeBmp = 1;
constexpr std::uint16_t windowsUnicodeFull = 10;
// Format 4, segment mapping to delta values: segCountX2 at byte 6, then arrays of segCount 16-bit numbers: endCode from
// byte 14, then after a 16-bit pad startCode, idDelta and idRangeOffset, then glyphIdArray.
constexpr std::uint16_t segmentFormat = 4;
constexpr std::size_t format4SegCountX2 = 6;
constexpr std::size_t format4EndCodes = 14;
// U+FFFF is a noncharacter, which format 4's last segment maps to glyph 0. Some fonts point that segment's
// idRangeOffset past the subtable, so format 4 is read only up to the character before it.
constexpr char32_t lastSegmentCharacter = 0xFFFE;
// Format 12, segmented coverage: numGroups at byte 12, then from byte 16 a 12-byte group: startCharCode, endCharCode
// and startGlyphID.
constexpr std::uint16_t groupFormat = 12;
constexpr std::size_t format12NumGroups = 12;
constexpr std::size_t format12Groups = 16;
constexpr std::size_t groupSize = 12;
constexpr std::size_t groupEndCharCode = 4;
constexpr std::size_t groupStartGlyphId = 8;
// The last Unicode code point.
constexpr char32_t lastCodePoint = 0x10FFFF;

// kern (OpenType, "kern - Kerning"): version 0 at byte 0 (Apple's layout has version 1 there), nTables at byte 2, then
// from byte 4 subtables, each with a 6-byte header: version, length and coverage.
constexpr std::uint16_t kernVersion = 0;
constexpr std::size_t kernTableCount = 2;
constexpr std::size_t kernSubtables = 4;
constexpr std::size_t subtableLength = 2;
constexpr std::size_t subtableCoverage = 4;
constexpr std::size_t subtableHeaderSize = 6;
// Coverage: bits 0 to 3 are horizontal, minimum, cross-stream and override, and the high byte is the format.
constexpr std::uint16_t kernHorizontal = 0x0001;
constexpr std::uint16_t kernMinimum = 0x0002;
constexpr std::uint16_t kernCrossStream = 0x0004;
constexpr std::uint16_t kernOverride = 0x0008;
constexpr int kernFormatShift = 8;
constexpr int kernPairFormat = 0;
// Format 0: nPairs at byte 6 of the subtable, then from byte 14 a 6-byte pair: left glyph, right glyph and value.
constexpr std::size_t format0NPairs = 6;
constexpr std::size_t format0Pairs = 14;
constexpr std::size_t kernPairSize = 6;
constexpr std::size_t kernPairValue = 4;

// UTF-8's well-formed byte sequences (the Unicode Standard, Table 3-7): a lead byte from first to last, then
// continuation more bytes, the first of them from low to high and the rest from continuationLow to continuationHigh.
struct Utf8Lead {
    std::uint8_t first{};
    std::uint8_t last{};
    std::size_t continuation{};
    std::uint8_t low{};
    std::uint8_t high{};
};
constexpr std::array<Utf8Lead, 8> utf8Leads{{
    {0xC2, 0xDF, 1, 0x80, 0xBF},
    {0xE0, 0xE0, 2, 0xA0, 0xBF},
    {0xE1, 0xEC, 2, 0x80, 0xBF},
    {0xED, 0xED, 2, 0x80, 0x9F},
    {0xEE, 0xEF, 2, 0x80, 0xBF},
    {0xF0, 0xF0, 3, 0x90, 0xBF},
    {0xF1, 0xF3, 3, 0x80, 0xBF},
    {0xF4, 0xF4, 3, 0x80, 0x8F},
}};
constexpr std::uint8_t continuationLow = 0x80;
constexpr std::uint8_t continuationHigh = 0xBF;
// A continuation byte carries 6 bits of the code point; a lead byte before n continuation bytes carries 6 - n.
constexpr int continuationBits = 6;
constexpr std::uint8_t continuationPayload = 0x3F;
// Bytes below this are ASCII characters, each a code point of its own.
constexpr std::uint8_t asciiEnd = 0x80;
constexpr char32_t replacementCharacter = 0xFFFD;

// Big-endian numbers read from one table's bytes, each read checked against them.
class Reader {
  public:
    Reader(std::span<const std::uint8_t> bytes, std::string_view table) : bytes_(bytes), table_(table) {}

    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }

    // The @p length bytes from @p at.
    [[nodiscard]] Reader slice(std::size_t at, std::size_t length) const {
        need(at, length);
        return {bytes_.subspan(at, length), table_};
    }
    // The bytes from @p at to the end.
    [[nodiscard]] Reader from(std::size_t at) const { return slice(at, at <= size() ? size() - at : 0); }
    // The @p count records of @p stride bytes each from @p at.
    [[nodiscard]] Reader records(std::size_t at, std::size_t count, std::size_t stride) const {
        if (at > size() || count > (size() - at) / stride)
            truncated();
        return {bytes_.subspan(at, count * stride), table_};
    }

    [[nodiscard]] std::uint8_t u8(std::size_t at) const {
        need(at, 1);
        return bytes_[at];
    }
    [[nodiscard]] std::uint16_t u16(std::size_t at) const {
        need(at, size16);
        return static_cast<std::uint16_t>(bytes_[at] << 8 | bytes_[at + 1]);
    }
    [[nodiscard]] std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const {
        need(at, size32);
        return std::uint32_t{bytes_[at]} << 24 | std::uint32_t{bytes_[at + 1]} << 16 |
               std::uint32_t{bytes_[at + 2]} << 8 | std::uint32_t{bytes_[at + 3]};
    }

    // Throws a FontError for this table that says @p problem.
    [[noreturn]] void fail(std::string_view problem) const {
        throw FontError(std::string(table_) + ": " + std::string(problem));
    }

  private:
    void need(std::size_t at, std::size_t length) const {
        if (at > size() || length > size() - at)
            truncated();
    }
    [[noreturn]] void truncated() const { fail("truncated"); }

    std::span<const std::uint8_t> bytes_;
    std::string_view table_;
};

// The table records of a TrueType font's table directory, each checked to lie within @p file.
Reader tableRecords(const Reader &file) {
    const std::uint32_t version = file.u32(0);
    if (version == cffVersion)
        throw FontError("CFF outlines are not supported: this is an OpenType font with a CFF table, not glyf");
    if (version == collectionTag)
        throw FontError("font collections are not supported");
    if (version != trueTypeVersion && version != appleTrueTypeVersion)
        throw FontError("not a TrueType font");
    const Reader records = file.records(directoryRecords, file.u16(directoryTableCount), directoryRecordSize);
    // Every table lies within the data, whether or not it is read.
    for (std::size_t at = 0; at < records.size(); at += directoryRecordSize)
        static_cast<void>(file.slice(records.u32(at + recordOffset), records.u32(at + recordLength)));
    return records;
}

// The tables of a font, as its table directory lists them.
class TableDirectory {
  public:
    explicit TableDirectory(std::span<const std::uint8_t> data)
        : file_(data, "table directory"), records_(tableRecords(file_)) {}

    // The table tagged @p tag, if the font has one.
    [[nodiscard]] std::optional<Reader> find(std::string_view tag) const {
        for (std::size_t at = 0; at < records_.size(); at += directoryRecordSize)
            if (records_.u32(at) == tagOf(tag))
                return Reader(file_.slice(records_.u32(at + recordOffset), records_.u32(at + recordLength)).bytes(),
                              tag);
        return std::nullopt;
    }
    [[nodiscard]] Reader require(std::string_view tag) const {
        if (const std::optional<Reader> table = find(tag))
            return *table;
        throw FontError("missing the required " + std::string(tag) + " table");
    }

  private:
    Reader file_;
    Reader records_;
};

std::vector<std::uint16_t> readAdvances(const Reader &hmtx, std::size_t numGlyphs, std::size_t numberOfHMetrics) {
    const Reader metrics = hmtx.records(0, numberOfHMetrics, longHorMetricSize);
    std::vector<std::uint16_t> advances(numGlyphs);
    for (std::size_t glyph = 0; glyph < numGlyphs; ++glyph)
        advances[glyph] = metrics.u16(std::min(glyph, numberOfHMetrics - 1) * longHorMetricSize);
    return advances;
}

std::vector<std::uint32_t> readGlyphOffsets(const Reader &loca, std::size_t numGlyphs, bool longForm,
                                            std::size_t glyfSize) {
    const std::size_t entrySize = longForm ? size32 : size16;
    const Reader entries = loca.records(0, numGlyphs + 1, entrySize);
    std::vector<std::uint32_t> offsets(numGlyphs + 1);
    for (std::size_t i = 0; i <= numGlyphs; ++i) {
        offsets[i] = longForm ? entries.u32(i * entrySize) : entries.u16(i * entrySize) * shortOffsetScale;
        if (offsets[i] > glyfSize || (i > 0 && offsets[i] < offsets[i - 1]))
            loca.fail("glyph offsets decrease or pass the end of glyf");
    }
    return offsets;
}

// A character map's ranges as a cmap subtable lists them, in order, each checked to map to glyphs the font has.
class CharacterRanges {
  public:
    CharacterRanges(Reader subtable, std::size_t numGlyphs) : subtable_(subtable), numGlyphs_(numGlyphs) {}

    // Characters @p first to @p last map to consecutive glyphs from @p glyph.
    void add(char32_t first, char32_t last, std::uint32_t glyph) {
        if (glyph >= numGlyphs_ || std::size_t{last - first} >= numGlyphs_ - glyph)
            subtable_.fail("a character maps to a glyph the font does not have");
        ranges_.push_back({first, last, static_cast<GlyphId>(glyph)});
    }
    [[nodiscard]] std::vector<detail::CharacterRange> take() && { return std::move(ranges_); }

  private:
    Reader subtable_;
    std::size_t numGlyphs_;
    std::vector<detail::CharacterRange> ranges_;
};

std::vector<detail::CharacterRange> readSegments(const Reader &subtable, std::size_t numGlyphs) {
    CharacterRanges ranges(subtable, numGlyphs);
    const std::size_t arraySize = subtable.u16(format4SegCountX2);
    const std::size_t segments = arraySize / size16;
    const Reader ends = subtable.records(format4EndCodes, segments, size16);
    const std::size_t startsAt = format4EndCodes + arraySize + size16;
    const Reader starts = subtable.records(startsAt, segments, size16);
    const Reader deltas = subtable.records(startsAt + arraySize, segments, size16);
    const std::size_t rangeOffsetsAt = startsAt + 2 * arraySize;
    const Reader rangeOffsets = subtable.records(rangeOffsetsAt, segments, size16);
    char32_t previousEnd = 0;
    for (std::size_t i = 0; i < segments; ++i) {
        const char32_t start = starts.u16(i * size16);
        const char32_t end = ends.u16(i * size16);
        if (start > end || (i > 0 && start <= previousEnd))
            subtable.fail("format 4 segments overlap or are out of order");
        previousEnd = end;
        const std::uint16_t delta = deltas.u16(i * size16);
        const std::size_t rangeOffset = rangeOffsets.u16(i * size16);
        const char32_t last = std::min(end, lastSegmentCharacter);
        if (rangeOffset == 0) {
            // Glyph ids add idDelta modulo 65536, so a segment's run of consecutive glyphs wraps to 0 at most once.
            for (char32_t c = start; c <= last;) {
                const auto glyph = static_cast<std::uint16_t>(c + delta);
                const char32_t room = char32_t{std::numeric_limits<std::uint16_t>::max()} - char32_t{glyph};
                const char32_t runLast = std::min<char32_t>(last, c + room);
                ranges.add(c, runLast, glyph);
                c = runLast + 1;
            }
        } else {
            // idRangeOffset counts bytes from itself to the segment's first entry in glyphIdArray. An entry's glyph id
            // adds idDelta too, except 0, which marks a character the font lacks.
            const std::size_t entries = rangeOffsetsAt + i * size16 + rangeOffset;
            for (char32_t c = start; c <= last; ++c) {
                const std::uint16_t entry = subtable.u16(entries + std::size_t{c - start} * size16);
                if (entry != 0)
                    ranges.add(c, c, static_cast<std::uint16_t>(entry + delta));
            }
        }
    }
    return std::move(ranges).take();
}

std::vector<detail::CharacterRange> readGroups(const Reader &subtable, std::size_t numGlyphs) {
    CharacterRanges ranges(subtable, numGlyphs);
    const Reader records = subtable.records(format12Groups, subtable.u32(format12NumGroups), groupSize);
    char32_t previousLast = 0;
    for (std::size_t at = 0; at < records.size(); at += groupSize) {
        const char32_t first = records.u32(at);
        const char32_t last = records.u32(at + groupEndCharCode);
        if (first > last || last > lastCodePoint || (at > 0 && first <= previousLast))
            subtable.fail("format 12 groups overlap, are out of order, or pass the last code point");
        previousLast = last;
        ranges.add(first, last, records.u32(at + groupStartGlyphId));
    }
    return std::move(ranges).take();
}

// The character map of the font's best Unicode subtable: format 12, which reaches past the Basic Multilingual Plane,
// over format 4, which does not.
std::vector<detail::CharacterRange> readCharacterMap(const Reader &cmap, std::size_t numGlyphs) {
    const Reader records = cmap.records(cmapRecords, cmap.u16(cmapTableCount), cmapRecordSize);
    std::optional<Reader> best;
    std::uint16_t bestFormat = 0;
    for (std::size_t at = 0; at < records.size(); at += cmapRecordSize) {
        const std::uint16_t platform = records.u16(at);
        const std::uint16_t encoding = records.u16(at + cmapRecordEncoding);
        const bool unicode =
            platform == unicodePlatform ||
            (platform == windowsPlatform && (encoding == windowsUnicodeBmp || encoding == windowsUnicodeFull));
        if (!unicode)
            continue;
        const Reader subtable = cmap.from(records.u32(at + cmapRecordOffset));
        const std::uint16_t format = subtable.u16(0);
        if ((format == groupFormat && bestFormat != groupFormat) || (format == segmentFormat && !best)) {
            best = subtable;
            bestFormat = format;
        }
    }
    if (!best)
        cmap.fail("no Unicode subtable in format 4 or 12");
    return bestFormat == groupFormat ? readGroups(*best, numGlyphs) : readSegments(*best, numGlyphs);
}

std::vector<detail::KernSubtable> readKerning(const Reader &kern) {
    std::vector<detail::KernSubtable> subtables;
    if (kern.u16(0) != kernVersion)
        return subtables;
    std::size_t at = kernSubtables;
    for (std::size_t i = kern.u16(kernTableCount); i > 0; --i) {
        const Reader subtable = kern.from(at);
        const std::size_t length = subtable.u16(subtableLength);
        const std::uint16_t coverage = subtable.u16(subtableCoverage);
        if (length < subtableHeaderSize)
            kern.fail("a subtable is shorter than its header");
        // Only horizontal kerning values change advances; minimum values and cross-stream shifts do not.
        const bool kernsAdvances = (coverage & (kernHorizontal | kernMinimum | kernCrossStream)) == kernHorizontal;
        if (coverage >> kernFormatShift == kernPairFormat && kernsAdvances) {
            // A format 0 subtable's 16-bit length overflows past 10920 pairs, so the pairs end where nPairs says,
            // within the table, rather than where length does.
            const Reader records = subtable.records(format0Pairs, subtable.u16(format0NPairs), kernPairSize);
            detail::KernSubtable parsed;
            parsed.replaces = (coverage & kernOverride) != 0;
            for (std::size_t pair = 0; pair < records.size(); pair += kernPairSize)
                parsed.pairs.push_back({records.u32(pair), records.i16(pair + kernPairValue)});
            // kern lists pairs in order of key, but fonts that do not still kern. Sorting keeps every pair, and of two
            // for the same glyphs the first, which a search through them in order finds.
            std::ranges::stable_sort(parsed.pairs, {}, &detail::KernPair::key);
            const auto repeated = std::ranges::unique(parsed.pairs, {}, &detail::KernPair::key);
            parsed.pairs.erase(repeated.begin(), repeated.end());
            subtables.push_back(std::move(parsed));
        }
        at += length;
    }
    return subtables;
}

// A glyph's points in font units, y up as TrueType has them, and where its contours end.
struct OutlinePoint {
    Vec2 at{};
    bool onCurve = false;
};
struct Outline {
    std::vector<OutlinePoint> points;
    // One past each contour's last point.
    std::vector<std::size_t> ends;
};

// Reads glyph outlines from glyf, resolving composite glyphs' components within bounds that keep a malformed font's
// work and memory in check.
class GlyphReader {
  public:
    GlyphReader(std::span<const std::uint8_t> glyf, std::span<const std::uint32_t> offsets)
        : glyf_(glyf, "glyf"), offsets_(offsets) {}

    // The outline of @p glyph, one the font has, @p depth components deep in the glyph being read.
    Outline read(std::size_t glyph, int depth) {
        const std::size_t begin = offsets_[glyph];
        const std::size_t end = offsets_[glyph + 1];
        if (begin == end)
            return {};
        const Reader data = glyf_.slice(begin, end - begin);
        const std::int16_t contours = data.i16(0);
        return contours >= 0 ? simple(data, static_cast<std::size_t>(contours)) : composite(data, depth);
    }

  private:
    Outline simple(const Reader &data, std::size_t contourCount) {
        Outline outline;
        const Reader endPoints = data.records(glyphHeaderSize, contourCount, size16);
        for (std::size_t i = 0; i < contourCount; ++i) {
            const std::size_t end = std::size_t{endPoints.u16(i * size16)} + 1;
            if (!outline.ends.empty() && end <= outline.ends.back())
                data.fail("a glyph's contours end out of order");
            outline.ends.push_back(end);
        }
        const std::size_t pointCount = outline.ends.empty() ? 0 : outline.ends.back();
        // Every point read ends up in the glyph being read, however deep its components nest.
        points_ += pointCount;
        if (points_ > mostGlyphPoints)
            data.fail("a glyph has more points than TrueType can number");
        const std::size_t instructionsAt = glyphHeaderSize + endPoints.size();
        std::size_t at = instructionsAt + size16 + data.u16(instructionsAt);

        std::vector<std::uint8_t> flags;
        flags.reserve(pointCount);
        while (flags.size() < pointCount) {
            const std::uint8_t flag = data.u8(at++);
            std::size_t repeat = 1;
            if ((flag & repeatFlag) != 0)
                repeat += data.u8(at++);
            if (repeat > pointCount - flags.size())
                data.fail("a glyph's flags repeat past its last point");
            flags.insert(flags.end(), repeat, flag);
        }

        outline.points.resize(pointCount);
        // Each coordinate is a change from the last: a byte whose sign a flag gives, or none, or a signed 16-bit one.
        const auto readAxis = [&](std::uint8_t shortVector, std::uint8_t sameOrPositive, double Vec2::*axis) {
            double value = 0.0;
            for (std::size_t i = 0; i < pointCount; ++i) {
                const bool positive = (flags[i] & sameOrPositive) != 0;
                if ((flags[i] & shortVector) != 0) {
                    const double change = data.u8(at++);
                    value += positive ? change : -change;
                } else if (!positive) {
                    value += data.i16(at);
                    at += size16;
                }
                outline.points[i].at.*axis = value;
            }
        };
        readAxis(xShortVector, xIsSameOrPositive, &Vec2::x);
        readAxis(yShortVector, yIsSameOrPositive, &Vec2::y);
        for (std::size_t i = 0; i < pointCount; ++i)
            outline.points[i].onCurve = (flags[i] & onCurvePoint) != 0;
        return outline;
    }

    Outline composite(const Reader &data, int depth) {
        if (depth == deepestComponentNesting)
            data.fail("composite glyphs nest too deeply");
        Outline outline;
        std::size_t at = glyphHeaderSize;
        std::uint16_t flags = moreComponents;
        while ((flags & moreComponents) != 0) {
            if (++components_ > mostGlyphComponents)
                data.fail("a glyph has more components than TrueType can count");
            // flags and glyphIndex, then two arguments: an offset's x and y, signed, or two point numbers, unsigned,
            // each in a byte or a 16-bit word.
            flags = data.u16(at);
            const std::size_t glyph = data.u16(at + size16);
            at += 2 * size16;
            if (glyph + 1 >= offsets_.size())
                data.fail("a component is a glyph the font does not have");
            const bool words = (flags & arg1And2AreWords) != 0;
            const bool xy = (flags & argsAreXyValues) != 0;
            const std::size_t argumentSize = words ? size16 : 1;
            const auto argument = [&](std::size_t index) -> int {
                const std::size_t argumentAt = at + index * argumentSize;
                if (words)
                    return xy ? int{data.i16(argumentAt)} : int{data.u16(argumentAt)};
                const std::uint8_t byte = data.u8(argumentAt);
                return xy ? int{static_cast<std::int8_t>(byte)} : int{byte};
            };
            const int first = argument(0);
            const int second = argument(1);
            at += 2 * argumentSize;

            const auto fixed = [&](std::size_t index) { return data.i16(at + index * size16) / f2dot14One; };
            Affine transform;
            if ((flags & weHaveAScale) != 0) {
                transform = scale(fixed(0));
                at += size16;
            } else if ((flags & weHaveAnXAndYScale) != 0) {
                transform = scale(fixed(0), fixed(1));
                at += 2 * size16;
            } else if ((flags & weHaveATwoByTwo) != 0) {
                // xscale, scale01, scale10 and yscale: x' = xscale x + scale10 y and y' = scale01 x + yscale y.
                transform = {fixed(0), fixed(1), fixed(2), fixed(3), 0.0, 0.0};
                at += 4 * size16;
            }

            Outline part = read(glyph, depth + 1);
            for (OutlinePoint &point : part.points)
                point.at = apply(transform, point.at);
            Vec2 offset{};
            if (xy) {
                offset = {static_cast<double>(first), static_cast<double>(second)};
                // Fonts made for Apple's rasterizer move a component in its transformed units, and those made for
                // Microsoft's, the default, in the glyph's.
                if ((flags & scaledComponentOffset) != 0 && (flags & unscaledComponentOffset) == 0)
                    offset = applyLinear(transform, offset);
            } else {
                // The component moves so that its point numbered second lands on the glyph's point numbered first.
                const auto target = static_cast<std::size_t>(first);
                const auto source = static_cast<std::size_t>(second);
                if (target >= outline.points.size() || source >= part.points.size())
                    data.fail("a component matches a point that does not exist");
                offset = outline.points[target].at - part.points[source].at;
            }

            const std::size_t base = outline.points.size();
            for (const OutlinePoint &point : part.points)
                outline.points.push_back({point.at + offset, point.onCurve});
            for (const std::size_t end : part.ends)
                outline.ends.push_back(base + end);
        }
        return outline;
    }

    Reader glyf_;
    std::span<const std::uint32_t> offsets_;
    // Points and components read so far, nested ones included.
    std::size_t points_ = 0;
    std::size_t components_ = 0;
};

// One contour, a TrueType quadratic spline, as a closed subpath with y flipped.
void appendContour(Path &path, std::span<const OutlinePoint> contour) {
    const auto flipped = [](const OutlinePoint &point) { return Vec2{point.at.x, -point.at.y}; };
    const OutlinePoint &head = contour.front();
    const OutlinePoint &tail = contour.back();
    Vec2 start{};
    std::span<const OutlinePoint> rest = contour;
    if (head.onCurve) {
        start = flipped(head);
        rest = contour.subspan(1);
    } else if (tail.onCurve) {
        start = flipped(tail);
        rest = contour.first(contour.size() - 1);
    } else {
        start = (flipped(head) + flipped(tail)) * 0.5;
    }
    path.moveTo(start);
    std::optional<Vec2> control;
    for (const OutlinePoint &point : rest) {
        const Vec2 p = flipped(point);
        if (point.onCurve) {
            if (control)
                path.quadTo(*control, p);
            else
                path.lineTo(p);
            control.reset();
        } else {
            if (control)
                path.quadTo(*control, (*control + p) * 0.5);
            control = p;
        }
    }
    if (control)
        path.quadTo(*control, start);
    path.close();
}

// The code point that @p text starts with, which it then drops. An ill-formed sequence decodes to U+FFFD and drops its
// maximal subpart: the longest start of a well-formed sequence that it has, or else its first byte.
char32_t takeCodePoint(std::string_view &text) {
    const auto byte = [&text](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const std::uint8_t lead = byte(0);
    if (lead < asciiEnd) {
        text.remove_prefix(1);
        return lead;
    }
    const auto form =
        std::ranges::find_if(utf8Leads, [lead](const Utf8Lead &l) { return lead >= l.first && lead <= l.last; });
    if (form == utf8Leads.end()) {
        text.remove_prefix(1);
        return replacementCharacter;
    }
    auto codePoint = static_cast<char32_t>(lead & (continuationPayload >> form->continuation));
    std::uint8_t low = form->low;
    std::uint8_t high = form->high;
    for (std::size_t i = 1; i <= form->continuation; ++i) {
        if (i == text.size() || byte(i) < low || byte(i) > high) {
            text.remove_prefix(i);
            return replacementCharacter;
        }
        codePoint = codePoint << continuationBits | static_cast<char32_t>(byte(i) & continuationPayload);
        low = continuationLow;
        high = continuationHigh;
    }
    text.remove_prefix(form->continuation + 1);
    return codePoint;
}

// Sets @p text in @p font as one line, calling @p place with each glyph and how far its origin is from the line's, in
// font units, and returns the line's advance in font units. The sums are whole numbers, which doubles hold exactly.
template <class Place> double layOut(const Font &font, std::string_view text, Place place) {
    double pen = 0.0;
    std::optional<GlyphId> previous;
    while (!text.empty()) {
        const GlyphId glyph = font.glyphFor(takeCodePoint(text));
        if (previous)
            pen += font.kerning(*previous, glyph);
        place(glyph, pen);
        pen += font.advance(glyph);
        previous = glyph;
    }
    return pen;
}

} // namespace

Font::Font(std::span<const std::uint8_t> data) {
    const TableDirectory tables(data);

    const Reader head = tables.require("head");
    if (head.u32(headMagicNumber) != headMagic)
        head.fail("wrong magic number");
    unitsPerEm_ = head.u16(headUnitsPerEm);
    if (unitsPerEm_ < fewestUnitsPerEm || unitsPerEm_ > mostUnitsPerEm)
        head.fail("unitsPerEm is outside 16 to 16384");
    const std::int16_t locaFormat = head.i16(headIndexToLocFormat);
    if (locaFormat != shortOffsets && locaFormat != longOffsets)
        head.fail("unknown indexToLocFormat");

    const Reader maxp = tables.require("maxp");
    const std::size_t numGlyphs = maxp.u16(maxpNumGlyphs);
    if (numGlyphs == 0)
        maxp.fail("the font has no glyphs");

    const Reader hhea = tables.require("hhea");
    ascender_ = hhea.i16(hheaAscender);
    descender_ = hhea.i16(hheaDescender);
    lineGap_ = hhea.i16(hheaLineGap);
    const std::size_t numberOfHMetrics = hhea.u16(hheaNumberOfHMetrics);
    if (numberOfHMetrics == 0 || numberOfHMetrics > numGlyphs)
        hhea.fail("numberOfHMetrics is 0 or more than the font's glyphs");
    advances_ = readAdvances(tables.require("hmtx"), numGlyphs, numberOfHMetrics);

    const Reader glyf = tables.require("glyf");
    glyphOffsets_ = readGlyphOffsets(tables.require("loca"), numGlyphs, locaFormat == longOffsets, glyf.size());
    glyf_.assign(glyf.bytes().begin(), glyf.bytes().end());

    characters_ = readCharacterMap(tables.require("cmap"), numGlyphs);
    if (const std::optional<Reader> kern = tables.find("kern"))
        kerning_ = readKerning(*kern);
}

GlyphId Font::glyphFor(char32_t codePoint) const {
    const auto after = std::ranges::upper_bound(characters_, codePoint, {}, &detail::CharacterRange::first);
    if (after == characters_.begin())
        return 0;
    const detail::CharacterRange range = *std::prev(after);
    return codePoint <= range.last ? static_cast<GlyphId>(range.glyph + (codePoint - range.first)) : GlyphId{0};
}

int Font::advance(GlyphId glyph) const { return glyph < advances_.size() ? advances_[glyph] : 0; }

int Font::kerning(GlyphId left, GlyphId right) const {
    const std::uint32_t key = std::uint32_t{left} << 16 | right;
    int value = 0;
    for (const detail::KernSubtable &subtable : kerning_) {
        const auto found = std::ranges::lower_bound(subtable.pairs, key, {}, &detail::KernPair::key);
        if (found != subtable.pairs.end() && found->key == key)
            value = subtable.replaces ? found->value : value + found->value;
    }
    return value;
}

Path Font::glyphPath(GlyphId glyph) const {
    if (glyph >= advances_.size())
        return {};
    const Outline outline = GlyphReader(glyf_, glyphOffsets_).read(glyph, 0);
    Path path;
    std::size_t first = 0;
    for (const std::size_t end : outline.ends) {
        appendContour(path, std::span(outline.points).subspan(first, end - first));
        first = end;
    }
    return path;
}

Path textPath(const Font &font, std::string_view text, double size, Vec2 origin) {
    const double perUnit = size / font.unitsPerEm();
    Path path;
    layOut(font, text, [&](GlyphId glyph, double pen) {
        path.append(
            transformed(font.glyphPath(glyph), translate({origin.x + pen * perUnit, origin.y}) * scale(perUnit)));
    });
    return path;
}

double textAdvance(const Font &font, std::string_view text, double size) {
    return layOut(font, text, [](GlyphId, double) {}) * size / font.unitsPerEm();
}

} // namespace linea
