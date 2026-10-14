// linea - PNG: images encoded as PNG files.
#pragma once

#include "linea/Raster.hpp"

#include <cstdint>
#include <vector>

namespace linea {

// @p image as the bytes of a PNG file: 8-bit truecolor with alpha, sRGB-encoded with straight alpha as an Image holds
// it, which viewers and browsers assume of a PNG without color space chunks. Rows are unfiltered and compressed
// losslessly with DEFLATE's fixed Huffman codes, runs of bytes that repeat the pixel before them or the row above them
// as back-references, or stored uncompressed where that would take fewer bytes. Equal images encode to equal bytes.
//
// linea reads and writes no files: write the bytes wherever they belong, as with any other data. Throws
// std::invalid_argument for an image without area, which PNG cannot hold, or whose rgba does not hold exactly 4 bytes
// for each of its pixels.
std::vector<std::uint8_t> encodePng(const Image &image);

} // namespace linea
