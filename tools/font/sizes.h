#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "draw/font.h"

// What a fonts image's pictures would take if they were kept in other
// ways than the image keeps them, which is as they are: for choosing a way,
// and the sizes of the ladder, from the fonts themselves. Nothing here
// changes an image, and the firmware reads none of these forms.
//
// The ways, for the pictures of one face:
//
//   as they are     two bits a pixel, each glyph's rows end to end and
//                   filled out to a byte: what the image has.
//   run-length      each glyph by itself, as runs of pixels of one level:
//                   a byte to a run, its level in the highest two bits and
//                   one less than its length, which is 64 at the most, in
//                   the other six (RunLength()).
//   DEFLATE         each glyph by itself, its bytes as they are compressed
//                   by zlib at its best, with nothing round the stream.
//   the best        each glyph by itself in whichever of the three is
//                   smallest for it. (Which it is has to be kept too: two
//                   bits a glyph, which a glyph's record has spare.)
//   in groups       the glyphs in the order the face has them, so many at a
//                   time, each group's bytes as they are compressed
//                   together by DEFLATE: 16, 64 and 256 to a group, and
//                   the whole face as one. To draw one glyph, its group has
//                   to be decompressed, and kept if the next is to be
//                   quick.
//
// A face's tables (its record, its ranges, its glyphs' records) are the
// same in all of them, but for whatever says where a group begins, which is
// not counted.

namespace bn::font_tool {

// A glyph's picture as runs. Empty for a picture of no pixels.
std::vector<std::uint8_t> RunLength(const draw::Glyph& glyph);

// `bytes` compressed by DEFLATE, with no header or checksum round it.
std::vector<std::uint8_t> Deflated(std::span<const std::uint8_t> bytes);

// Bytes of pictures, in each of the ways.
struct PictureSizes {
  std::size_t asTheyAre = 0;
  std::size_t runLength = 0;
  std::size_t deflate = 0;
  std::size_t best = 0;
  std::size_t groupsOf16 = 0;
  std::size_t groupsOf64 = 0;
  std::size_t groupsOf256 = 0;
  std::size_t wholeFaces = 0;
  // How many glyphs were smallest in each way, among those kept by itself:
  // as they are (one with no pixels among them), as runs, and by DEFLATE.
  std::size_t bestAsTheyAre = 0;
  std::size_t bestRunLength = 0;
  std::size_t bestDeflate = 0;

  PictureSizes& operator+=(const PictureSizes& other) noexcept;
};

// The faces of one size, taken together.
struct SizeOfFaces {
  std::uint16_t size = 0;  // Pixels to the em.
  std::size_t faces = 0;
  std::size_t glyphs = 0;
  PictureSizes pictures;
};

struct ImageSizes {
  bool ok = false;
  std::string error;               // If not: why the image could not be gone through.
  std::size_t imageBytes = 0;      // As the image says it is long.
  std::size_t tableBytes = 0;      // All of it that is not pictures.
  std::vector<SizeOfFaces> sizes;  // In order of size.
  SizeOfFaces all;                 // Every face; its `size` is 0.
};

// Goes through every glyph of `image`.
ImageSizes MeasureImage(std::span<const std::uint8_t> image);

}  // namespace bn::font_tool
