// linea - PNG encoding, after the PNG specification (third edition), whose sections the constants below cite: an
// image's rows as a zlib stream (RFC 1950) of DEFLATE data (RFC 1951) in PNG's IDAT chunks.
#include "linea/Png.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace linea {
namespace {

using Bytes = std::vector<std::uint8_t>;

// The bytes every PNG file starts with (section 5.2).
constexpr std::array<std::uint8_t, 8> pngSignature{137, 80, 78, 71, 13, 10, 26, 10};

// IHDR's fields after the width and height (section 11.2.1): 8 bits a sample, color type 6, truecolor with alpha, and
// method 0 for compression, DEFLATE, for filtering, by a filter type on each row, and for interlacing, none.
constexpr std::uint8_t bitDepth = 8;
constexpr std::uint8_t truecolorWithAlpha = 6;
constexpr std::uint8_t deflateCompression = 0;
constexpr std::uint8_t adaptiveFiltering = 0;
constexpr std::uint8_t noInterlace = 0;
// The filter type that leaves a row's bytes as they are, which starts each row (section 9.2).
constexpr std::uint8_t noFilter = 0;
constexpr std::size_t bytesPerPixel = 4;

// A chunk may hold up to 2^31 - 1 bytes (section 5.3). The zlib stream goes in IDAT chunks of at most 64 KiB, so that
// a decoder can check each chunk's CRC before taking in more, at 12 bytes of overhead each.
constexpr std::size_t mostIdatBytes = std::size_t{1} << 16;

// CRC-32 over each chunk's type and data, with ISO 3309's polynomial, reflected (section 5.5 and Annex D).
constexpr std::uint32_t crcPolynomial = 0xEDB88320;
constexpr std::uint32_t crcInverted = 0xFFFFFFFF;
constexpr int bitsPerByte = 8;

constexpr std::array<std::uint32_t, 256> crcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < table.size(); ++n) {
        std::uint32_t c = n;
        for (int bit = 0; bit < bitsPerByte; ++bit)
            c = (c & 1) != 0 ? crcPolynomial ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}();

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t c = crcInverted;
    for (const std::uint8_t byte : bytes)
        c = crcTable[static_cast<std::uint8_t>(c ^ byte)] ^ (c >> bitsPerByte);
    return c ^ crcInverted;
}

// Adler-32 over the uncompressed data, which ends a zlib stream (RFC 1950, section 8.2): two sums modulo the largest
// prime below 2^16, the second in the high 16 bits. Reducing them only after every 5552 bytes, the most whose sums
// cannot overflow 32 bits from below the modulus, as zlib's NMAX, gives the same sums.
constexpr std::uint32_t adlerModulus = 65521;
constexpr std::size_t adlerRun = 5552;
constexpr int adlerHigh = 16;

std::uint32_t adler32(std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    while (!bytes.empty()) {
        const std::span<const std::uint8_t> run = bytes.first(std::min(bytes.size(), adlerRun));
        for (const std::uint8_t byte : run) {
            a += byte;
            b += a;
        }
        a %= adlerModulus;
        b %= adlerModulus;
        bytes = bytes.subspan(run.size());
    }
    return b << adlerHigh | a;
}

// Appends @p value most significant byte first, as PNG writes every number (section 7.1).
void appendBigEndian(Bytes &out, std::uint32_t value) {
    for (int shift = std::numeric_limits<std::uint32_t>::digits - bitsPerByte; shift >= 0; shift -= bitsPerByte)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// Appends a chunk: its data's length, its type, its data and the CRC of its type and data (section 5.3).
void appendChunk(Bytes &out, std::string_view type, std::span<const std::uint8_t> data) {
    appendBigEndian(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    appendBigEndian(out, crc32(std::span(out).subspan(start)));
}

// A zlib stream's two header bytes (RFC 1950, section 2.2): DEFLATE with a 32 KiB window, then FLEVEL 0, the fastest
// kind of compressor, which this is, no preset dictionary, and the check bits that make the two, read as a big-endian
// number, a multiple of 31.
constexpr std::uint8_t zlibMethod = 0x78;
constexpr unsigned zlibCheck = 31;
constexpr auto zlibFlags = static_cast<std::uint8_t>(zlibCheck - (unsigned{zlibMethod} << bitsPerByte) % zlibCheck);

// DEFLATE's limits (RFC 1951, sections 3.2.4 and 3.2.5): a back-reference copies 3 to 258 bytes from up to 32 KiB back,
// and a stored block holds up to 65535 bytes.
constexpr std::size_t shortestMatch = 3;
constexpr std::size_t longestMatch = 258;
constexpr std::size_t window = std::size_t{1} << 15;
constexpr std::size_t mostStoredBytes = 0xFFFF;
// A stored block's header takes a byte of its own, and its length and the length's complement two each.
constexpr std::size_t storedOverhead = 5;

// A block's header (RFC 1951, section 3.2.3): BFINAL, set on the last block, then the block's type.
constexpr std::uint32_t lastBlock = 1;
constexpr int lastBlockBits = 1;
constexpr std::uint32_t storedBlock = 0;
constexpr std::uint32_t fixedBlock = 1;
constexpr int blockTypeBits = 2;

// The codes of the lengths a back-reference copies, from 257 up: each code's least length and how many extra bits
// follow it to add to that (RFC 1951, section 3.2.5). Length 258 has a code of its own.
constexpr int firstLengthCode = 257;
constexpr std::array<std::size_t, 29> lengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> lengthExtraBits{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
// The codes of the distances a back-reference reaches back, from 0 up, likewise.
constexpr std::array<std::size_t, 30> distanceBase{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                   33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> distanceExtraBits{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr int endOfBlock = 256;

// DEFLATE's fixed Huffman codes for literals, the end of a block and lengths (RFC 1951, section 3.2.6): the symbols
// from first on take codes of so many bits, counting up from code.
struct FixedCodes {
    int first{};
    int bits{};
    std::uint32_t code{};
};
constexpr std::array<FixedCodes, 4> fixedLiteralCodes{{
    {0, 8, 0b00110000},
    {144, 9, 0b110010000},
    {256, 7, 0b0000000},
    {280, 8, 0b11000000},
}};
// Every distance code takes 5 bits, the code itself.
constexpr int fixedDistanceBits = 5;

// Bits packed into bytes from each byte's least significant bit up, as DEFLATE packs them (RFC 1951, section 3.1.1).
class BitWriter {
  public:
    // Appends the low @p count bits of @p value, least significant first, as DEFLATE writes numbers other than
    // Huffman codes. @p count is at most 24, so that what is pending fits 32 bits.
    void bits(std::uint32_t value, int count) {
        pending_ |= value << filled_;
        filled_ += count;
        while (filled_ >= bitsPerByte) {
            bytes_.push_back(static_cast<std::uint8_t>(pending_));
            pending_ >>= bitsPerByte;
            filled_ -= bitsPerByte;
        }
    }

    // Appends the Huffman code @p value of @p count bits, most significant first, as DEFLATE writes codes.
    void code(std::uint32_t value, int count) {
        std::uint32_t reversed = 0;
        for (int i = 0; i < count; ++i)
            reversed |= ((value >> i) & 1) << (count - 1 - i);
        bits(reversed, count);
    }

    // The bytes written, the last one padded with zero bits.
    Bytes finish() && {
        if (filled_ > 0)
            bytes_.push_back(static_cast<std::uint8_t>(pending_));
        return std::move(bytes_);
    }

  private:
    Bytes bytes_;
    std::uint32_t pending_ = 0;
    int filled_ = 0;
};

// Writes the fixed code of @p symbol, a literal byte, the end of a block or a length's code.
void writeSymbol(BitWriter &out, int symbol) {
    const auto codes = std::ranges::find_if(fixedLiteralCodes.rbegin(), fixedLiteralCodes.rend(),
                                            [&](const FixedCodes &range) { return range.first <= symbol; });
    out.code(codes->code + static_cast<std::uint32_t>(symbol - codes->first), codes->bits);
}

// The entry of a table whose least values are @p base that holds @p value: the last whose least value is no more.
template <std::size_t N> std::size_t entryOf(const std::array<std::size_t, N> &base, std::size_t value) {
    const auto above = std::ranges::upper_bound(base, value);
    return static_cast<std::size_t>(above - base.begin()) - 1;
}

// Writes a back-reference that copies @p length bytes from @p distance back: each one's code and extra bits.
void writeMatch(BitWriter &out, std::size_t length, std::size_t distance) {
    const std::size_t lengthCode = entryOf(lengthBase, length);
    writeSymbol(out, firstLengthCode + static_cast<int>(lengthCode));
    out.bits(static_cast<std::uint32_t>(length - lengthBase[lengthCode]), lengthExtraBits[lengthCode]);
    const std::size_t distanceCode = entryOf(distanceBase, distance);
    out.code(static_cast<std::uint32_t>(distanceCode), fixedDistanceBits);
    out.bits(static_cast<std::uint32_t>(distance - distanceBase[distanceCode]), distanceExtraBits[distanceCode]);
}

// How many of the bytes of @p data from @p at on repeat those @p distance before them, up to longestMatch.
std::size_t matchLength(std::span<const std::uint8_t> data, std::size_t at, std::size_t distance) {
    if (distance > at || distance > window)
        return 0;
    const std::size_t most = std::min(longestMatch, data.size() - at);
    std::size_t length = 0;
    while (length < most && data[at + length] == data[at + length - distance])
        ++length;
    return length;
}

// @p rows, @p stride bytes each, as one DEFLATE block with fixed Huffman codes. A rendered image repeats itself most
// in runs of one color and in rows like the row above, so at each byte the block takes the longer of the runs that
// repeat the pixel before it and the row above it, as a back-reference, where that run holds at least shortestMatch
// bytes, and the byte itself otherwise.
Bytes fixedDeflate(std::span<const std::uint8_t> rows, std::size_t stride) {
    BitWriter out;
    out.bits(lastBlock, lastBlockBits);
    out.bits(fixedBlock, blockTypeBits);
    for (std::size_t at = 0; at < rows.size();) {
        const std::size_t before = matchLength(rows, at, bytesPerPixel);
        const std::size_t above = matchLength(rows, at, stride);
        const std::size_t length = std::max(before, above);
        if (length < shortestMatch) {
            writeSymbol(out, rows[at]);
            ++at;
            continue;
        }
        // A tie takes the pixel before, the nearer, whose distance takes fewer bits.
        writeMatch(out, length, before >= above ? bytesPerPixel : stride);
        at += length;
    }
    writeSymbol(out, endOfBlock);
    return std::move(out).finish();
}

// @p data in stored blocks, uncompressed (RFC 1951, section 3.2.4).
Bytes storedDeflate(std::span<const std::uint8_t> data) {
    Bytes out;
    do {
        const std::span<const std::uint8_t> block = data.first(std::min(data.size(), mostStoredBytes));
        data = data.subspan(block.size());
        // BFINAL in the lowest bit, then the block type, padded to the byte's end.
        out.push_back(static_cast<std::uint8_t>((data.empty() ? lastBlock : 0U) | storedBlock << lastBlockBits));
        const auto length = static_cast<std::uint16_t>(block.size());
        for (const std::uint16_t half : {length, static_cast<std::uint16_t>(~length)}) {
            out.push_back(static_cast<std::uint8_t>(half));
            out.push_back(static_cast<std::uint8_t>(half >> bitsPerByte));
        }
        out.insert(out.end(), block.begin(), block.end());
    } while (!data.empty());
    return out;
}

// @p rows, @p stride bytes each, as a zlib stream: compressed, or stored where that takes fewer bytes.
Bytes zlibStream(std::span<const std::uint8_t> rows, std::size_t stride) {
    Bytes deflated = fixedDeflate(rows, stride);
    const std::size_t storedBlocks = (rows.size() + mostStoredBytes - 1) / mostStoredBytes;
    if (deflated.size() > rows.size() + storedBlocks * storedOverhead)
        deflated = storedDeflate(rows);
    Bytes stream{zlibMethod, zlibFlags};
    stream.insert(stream.end(), deflated.begin(), deflated.end());
    appendBigEndian(stream, adler32(rows));
    return stream;
}

} // namespace

std::vector<std::uint8_t> encodePng(const Image &image) {
    if (image.width <= 0 || image.height <= 0)
        throw std::invalid_argument("a PNG image's width and height must be at least 1");
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    // Divided rather than multiplied, so that no product can overflow.
    const std::size_t pixels = image.rgba.size() / bytesPerPixel;
    if (image.rgba.size() % bytesPerPixel != 0 || pixels % width != 0 || pixels / width != height)
        throw std::invalid_argument("an image's rgba must hold 4 bytes for each of its width by height pixels");
    const std::size_t rowBytes = width * bytesPerPixel;

    const std::size_t stride = 1 + rowBytes;
    Bytes rows;
    rows.reserve(height * stride);
    for (std::size_t y = 0; y < height; ++y) {
        rows.push_back(noFilter);
        const auto row = image.rgba.begin() + static_cast<std::ptrdiff_t>(y * rowBytes);
        rows.insert(rows.end(), row, row + static_cast<std::ptrdiff_t>(rowBytes));
    }

    Bytes png(pngSignature.begin(), pngSignature.end());
    Bytes header;
    appendBigEndian(header, static_cast<std::uint32_t>(image.width));
    appendBigEndian(header, static_cast<std::uint32_t>(image.height));
    header.insert(header.end(), {bitDepth, truecolorWithAlpha, deflateCompression, adaptiveFiltering, noInterlace});
    appendChunk(png, "IHDR", header);
    const Bytes stream = zlibStream(rows, stride);
    for (std::size_t at = 0; at < stream.size(); at += mostIdatBytes)
        appendChunk(png, "IDAT", std::span(stream).subspan(at, std::min(mostIdatBytes, stream.size() - at)));
    appendChunk(png, "IEND", {});
    return png;
}

} // namespace linea
