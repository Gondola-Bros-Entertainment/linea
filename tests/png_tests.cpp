#include "linea/Drawing.hpp"
#include "linea/Png.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace linea;

namespace {

using Bytes = std::vector<std::uint8_t>;

// A reference decoder for what encodePng writes, written apart from it from the PNG specification and RFCs 1950 and
// 1951: 8-bit truecolor with alpha, no interlace, every row with filter type 0, and a zlib stream of stored and fixed
// Huffman blocks. It checks every chunk's CRC and length, every header field, every back-reference against the data
// and the window, and the Adler-32, and fails the test on anything else.

// Fails the decoding, and so the test, with @p problem unless @p holds: an exception rather than an assertion, since
// the decoder checks every bit it reads.
void expect(bool holds, const char *problem) {
    if (!holds)
        throw std::runtime_error(problem);
}

// CRC-32 bit by bit, where the encoder uses a table.
std::uint32_t crcOf(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return ~crc;
}

// Adler-32, reduced after every byte, where the encoder reduces after thousands.
std::uint32_t adlerOf(std::span<const std::uint8_t> bytes) {
    constexpr std::uint32_t modulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % modulus;
        b = (b + a) % modulus;
    }
    return b << 16 | a;
}

std::span<const std::uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::uint32_t bigEndian(std::span<const std::uint8_t> bytes) {
    REQUIRE(bytes.size() >= 4);
    return std::uint32_t{bytes[0]} << 24 | std::uint32_t{bytes[1]} << 16 | std::uint32_t{bytes[2]} << 8 | bytes[3];
}

// Reads bits from each byte's least significant up, as DEFLATE packs them.
class BitReader {
  public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    // The next @p count bits as a number, the first read least significant.
    std::uint32_t bits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            expect(at_ < bytes_.size() * 8, "the data ends within a block");
            value |= static_cast<std::uint32_t>((bytes_[at_ / 8] >> (at_ % 8)) & 1) << i;
            ++at_;
        }
        return value;
    }

    // Skips to the start of the next byte, and gives its index.
    std::size_t toByte() {
        at_ = (at_ + 7) / 8 * 8;
        return at_ / 8;
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
};

// A canonical Huffman code, from each symbol's code length (RFC 1951, section 3.2.2): how many codes each length has,
// and the symbols in the order of their codes.
struct Huffman {
    std::array<int, 16> counts{};
    std::vector<int> symbols;
};

Huffman huffmanOf(const std::vector<int> &lengths) {
    Huffman code;
    for (int length = 1; length < 16; ++length) {
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] == length) {
                ++code.counts[static_cast<std::size_t>(length)];
                code.symbols.push_back(static_cast<int>(symbol));
            }
        }
    }
    return code;
}

// Reads one symbol of @p code: the codes of each length follow on from those one bit shorter, doubled.
int symbolOf(BitReader &in, const Huffman &code) {
    int value = 0;
    int first = 0;
    int index = 0;
    for (std::size_t length = 1; length < code.counts.size(); ++length) {
        value |= static_cast<int>(in.bits(1));
        const int count = code.counts[length];
        if (value - first < count)
            return code.symbols[static_cast<std::size_t>(index + value - first)];
        index += count;
        first = (first + count) << 1;
        value <<= 1;
    }
    throw std::runtime_error("a code that the Huffman code does not hold");
}

// The fixed codes (RFC 1951, section 3.2.6), and the lengths and distances their codes stand for, each the one before
// it plus the span its extra bits cover (section 3.2.5).
struct FixedCodes {
    Huffman literals;
    Huffman distances;
    std::vector<int> lengthBase;
    std::vector<int> lengthExtra;
    std::vector<int> distanceBase;
    std::vector<int> distanceExtra;
};

const FixedCodes &fixedCodes() {
    static const FixedCodes codes = [] {
        FixedCodes made;
        std::vector<int> lengths(288);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
            lengths[symbol] = symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
        made.literals = huffmanOf(lengths);
        made.distances = huffmanOf(std::vector<int>(30, 5));
        int length = 3;
        for (int i = 0; i < 28; ++i) {
            const int extra = i < 8 ? 0 : (i - 4) / 4;
            made.lengthBase.push_back(length);
            made.lengthExtra.push_back(extra);
            length += 1 << extra;
        }
        made.lengthBase.push_back(258);
        made.lengthExtra.push_back(0);
        int distance = 1;
        for (int i = 0; i < 30; ++i) {
            const int extra = i < 4 ? 0 : (i - 2) / 2;
            made.distanceBase.push_back(distance);
            made.distanceExtra.push_back(extra);
            distance += 1 << extra;
        }
        return made;
    }();
    return codes;
}

constexpr std::size_t windowSize = 32768;

// What a zlib stream inflates to, and the types of the DEFLATE blocks it held.
struct Inflated {
    Bytes data;
    std::vector<std::uint32_t> blockTypes;
};

Inflated inflate(std::span<const std::uint8_t> stream) {
    REQUIRE(stream.size() >= 6);
    // CM 8, DEFLATE, a window of at most 32 KiB, no preset dictionary, and the header a multiple of 31.
    CHECK((stream[0] & 0x0F) == 8);
    CHECK((stream[0] >> 4) <= 7);
    CHECK((stream[1] & 0x20) == 0);
    CHECK((stream[0] * 256 + stream[1]) % 31 == 0);
    BitReader in(stream.subspan(2));
    const FixedCodes &fixed = fixedCodes();
    Inflated out;
    bool last = false;
    while (!last) {
        last = in.bits(1) == 1;
        const std::uint32_t type = in.bits(2);
        out.blockTypes.push_back(type);
        if (type == 0) {
            in.toByte();
            const std::uint32_t length = in.bits(16);
            CHECK(in.bits(16) == (~length & 0xFFFF));
            for (std::uint32_t i = 0; i < length; ++i)
                out.data.push_back(static_cast<std::uint8_t>(in.bits(8)));
            continue;
        }
        REQUIRE(type == 1);
        while (true) {
            const int symbol = symbolOf(in, fixed.literals);
            if (symbol < 256) {
                out.data.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == 256)
                break;
            const auto lengthCode = static_cast<std::size_t>(symbol - 257);
            expect(lengthCode < fixed.lengthBase.size(), "a length code past 285");
            const auto length =
                static_cast<std::size_t>(fixed.lengthBase[lengthCode]) + in.bits(fixed.lengthExtra[lengthCode]);
            const auto distanceCode = static_cast<std::size_t>(symbolOf(in, fixed.distances));
            const auto distance =
                static_cast<std::size_t>(fixed.distanceBase[distanceCode]) + in.bits(fixed.distanceExtra[distanceCode]);
            expect(distance <= out.data.size(), "a back-reference to before the data");
            expect(distance <= windowSize, "a back-reference past the window");
            for (std::size_t i = 0; i < length; ++i)
                out.data.push_back(out.data[out.data.size() - distance]);
        }
    }
    const std::size_t end = 2 + in.toByte();
    REQUIRE(stream.size() == end + 4);
    CHECK(bigEndian(stream.subspan(end)) == adlerOf(out.data));
    return out;
}

// What a PNG file decodes to, and how it was stored.
struct Decoded {
    Image image;
    std::size_t idatChunks{};
    std::vector<std::uint32_t> blockTypes;
};

Decoded decode(std::span<const std::uint8_t> png) {
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    REQUIRE(png.size() >= signature.size());
    REQUIRE(std::equal(signature.begin(), signature.end(), png.begin()));
    std::vector<std::string> types;
    Bytes header;
    Bytes stream;
    Decoded decoded;
    for (std::size_t at = signature.size(); at < png.size();) {
        REQUIRE(png.size() - at >= 12);
        const std::uint32_t length = bigEndian(png.subspan(at));
        REQUIRE(length <= 0x7FFFFFFF);
        REQUIRE(png.size() - at - 12 >= length);
        const std::span<const std::uint8_t> typeAndData = png.subspan(at + 4, 4 + std::size_t{length});
        const std::string type(typeAndData.begin(), typeAndData.begin() + 4);
        const std::span<const std::uint8_t> data = typeAndData.subspan(4);
        CHECK(bigEndian(png.subspan(at + 8 + length)) == crcOf(typeAndData));
        if (type == "IHDR")
            header.assign(data.begin(), data.end());
        if (type == "IDAT") {
            // IDAT chunks follow IHDR and one another.
            REQUIRE_FALSE(types.empty());
            CHECK((types.back() == "IHDR" || types.back() == "IDAT"));
            stream.insert(stream.end(), data.begin(), data.end());
            ++decoded.idatChunks;
        }
        if (type == "IEND")
            CHECK(data.empty());
        types.push_back(type);
        at += 12 + std::size_t{length};
    }
    REQUIRE(types.size() >= 3);
    CHECK(types.front() == "IHDR");
    CHECK(types.back() == "IEND");
    for (std::size_t i = 1; i + 1 < types.size(); ++i)
        CHECK(types[i] == "IDAT");

    REQUIRE(header.size() == 13);
    const std::uint32_t width = bigEndian(header);
    const std::uint32_t height = bigEndian(std::span(header).subspan(4));
    REQUIRE((width >= 1 && width <= 0x7FFFFFFF && height >= 1 && height <= 0x7FFFFFFF));
    // 8 bits a sample, truecolor with alpha, DEFLATE, filtering by row, no interlace.
    CHECK(header[8] == 8);
    CHECK(header[9] == 6);
    CHECK(header[10] == 0);
    CHECK(header[11] == 0);
    CHECK(header[12] == 0);

    Inflated rows = inflate(stream);
    decoded.blockTypes = std::move(rows.blockTypes);
    const std::size_t rowBytes = std::size_t{width} * 4;
    REQUIRE(rows.data.size() == std::size_t{height} * (1 + rowBytes));
    decoded.image = {static_cast<int>(width), static_cast<int>(height), {}};
    for (std::size_t y = 0; y < height; ++y) {
        const auto row = rows.data.begin() + static_cast<std::ptrdiff_t>(y * (1 + rowBytes));
        CHECK(*row == 0);
        decoded.image.rgba.insert(decoded.image.rgba.end(), row + 1, row + 1 + static_cast<std::ptrdiff_t>(rowBytes));
    }
    return decoded;
}

void checkRoundTrip(const Decoded &decoded, const Image &image) {
    CHECK(decoded.image.width == image.width);
    CHECK(decoded.image.height == image.height);
    CHECK(decoded.image.rgba == image.rgba);
}

// An image of @p width by @p height pixels of seeded noise, each byte one of @p levels values from 0. Noise of every
// byte value does not compress; noise of few values repeats itself in short, scattered runs.
Image noise(int width, int height, std::uint32_t seed, std::uint32_t levels = 256) {
    std::mt19937 random(seed);
    Image image{width, height, Bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)};
    for (std::uint8_t &byte : image.rgba)
        byte = static_cast<std::uint8_t>(random() % levels);
    return image;
}

// A star filled with a gradient and stroked, over transparency, as the README draws it.
Image badge(int pixels) {
    Layer gem;
    gem.path = star({50.0, 50.0}, 44.0, 20.0, 5, -std::numbers::pi / 2.0);
    gem.fill = RadialGradient{{50.0, 40.0}, 48.0, evenStops({{255, 236, 160, 255}, {214, 120, 24, 255}})};
    gem.stroke = Paint{Color{40, 24, 12, 255}};
    gem.strokeStyle = {.width = 4.0, .join = Join::round};
    return render(Drawing{100.0, 100.0, {gem}}, pixels, pixels);
}

} // namespace

TEST_CASE("The reference decoder's checksums give their standard check values") {
    CHECK(crcOf(bytesOf("123456789")) == 0xCBF43926);
    CHECK(adlerOf(bytesOf("Wikipedia")) == 0x11E60398);
}

TEST_CASE("A pixel encodes byte for byte as zlib encodes it with fixed Huffman codes") {
    // Python's zlib, compressing the row with the Z_FIXED strategy at level 1 or 9, and its crc32 give these bytes.
    const Bytes expected{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
                         0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
                         0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
                         0x01, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x7b, 0xe9,
                         0xbe, 0xef, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    CHECK(encodePng(Image{1, 1, {255, 0, 0, 255}}) == expected);
}

TEST_CASE("A rendered image encodes as a PNG that decodes to its pixels, compressed") {
    const Image image = badge(64);
    const Bytes png = encodePng(image);
    const Decoded decoded = decode(png);
    checkRoundTrip(decoded, image);
    CHECK(decoded.blockTypes == std::vector<std::uint32_t>{1});
    CHECK(png.size() < image.rgba.size() / 2);
    // Equal images encode to equal bytes.
    CHECK(encodePng(badge(64)) == png);
}

TEST_CASE("An image of one color compresses to a few bytes a row") {
    // Each row of 1201 bytes repeats the row above and the pixel before in runs of up to 258 bytes. Every byte but the
    // rows' filter types is 255, the largest, which grows Adler-32's sums fastest.
    constexpr std::size_t side = 300;
    const Image flat{side, side, Bytes(side * side * 4, 255)};
    const Bytes png = encodePng(flat);
    checkRoundTrip(decode(png), flat);
    CHECK(png.size() < side * 16);
}

TEST_CASE("Noise of few values, full of short runs before and above, decodes to its pixels") {
    for (const int width : {1, 5, 100}) {
        for (const std::uint32_t levels : {2U, 3U}) {
            CAPTURE(width);
            CAPTURE(levels);
            const Image image = noise(width, 40, 3, levels);
            const Bytes png = encodePng(image);
            const Decoded decoded = decode(png);
            checkRoundTrip(decoded, image);
        }
    }
}

TEST_CASE("Noise is stored, in blocks of at most 65535 bytes and IDAT chunks of at most 64 KiB") {
    // 200 rows of 401 bytes: 80200 bytes, two stored blocks, and a zlib stream that spans two IDAT chunks.
    constexpr std::size_t rowsBytes = 80200;
    const Image image = noise(100, 200, 7);
    const Bytes png = encodePng(image);
    const Decoded decoded = decode(png);
    checkRoundTrip(decoded, image);
    CHECK(decoded.blockTypes == std::vector<std::uint32_t>{0, 0});
    CHECK(decoded.idatChunks == 2);
    // Nothing beyond what stored blocks need: the signature, IHDR, two IDAT chunks around a zlib stream of the rows,
    // two stored block headers and the Adler-32, and IEND.
    CHECK(png.size() == 8 + (12 + 13) + 2 * 12 + (2 + rowsBytes + 2 * 5 + 4) + 12);
}

TEST_CASE("A row repeats the row above by back-references that reach no farther than DEFLATE's window") {
    // A row of noise twice. 8191 pixels a row, the second row starts 32765 bytes after the first, within the 32 KiB a
    // back-reference reaches, so it compresses to a few bytes; at 8192 pixels it starts 32769 bytes after, out of
    // reach, and the image does not compress, so it is stored.
    for (const int width : {8191, 8192}) {
        CAPTURE(width);
        Image image = noise(width, 1, 11);
        image.height = 2;
        image.rgba.insert(image.rgba.end(), image.rgba.begin(), image.rgba.end());
        const Bytes png = encodePng(image);
        const Decoded decoded = decode(png);
        checkRoundTrip(decoded, image);
        const auto rowBytes = static_cast<std::size_t>(width) * 4;
        if (width == 8191) {
            CHECK(decoded.blockTypes == std::vector<std::uint32_t>{1});
            CHECK(png.size() < rowBytes * 5 / 4);
        } else {
            // Two rows of 32769 bytes take two stored blocks.
            CHECK(decoded.blockTypes == std::vector<std::uint32_t>{0, 0});
        }
    }
}

TEST_CASE("encodePng refuses an image without area, or whose bytes do not match its size") {
    CHECK_THROWS_AS(encodePng(Image{}), std::invalid_argument);
    CHECK_THROWS_AS(encodePng(Image{0, 1, {}}), std::invalid_argument);
    CHECK_THROWS_AS(encodePng(Image{1, -1, {}}), std::invalid_argument);
    // Bytes that are not whole pixels, pixels that are not whole rows, and whole rows of the wrong count.
    CHECK_THROWS_AS(encodePng(Image{2, 2, Bytes(17)}), std::invalid_argument);
    CHECK_THROWS_AS(encodePng(Image{2, 2, Bytes(12)}), std::invalid_argument);
    CHECK_THROWS_AS(encodePng(Image{2, 2, Bytes(8)}), std::invalid_argument);
    CHECK_THROWS_AS(encodePng(Image{2, 2, Bytes(24)}), std::invalid_argument);
    CHECK_NOTHROW(encodePng(Image{2, 2, Bytes(16)}));
}
