#include "font/sizes.h"

#include <zlib.h>

#include <array>
#include <map>

namespace bn::font_tool {

namespace {

constexpr std::uint32_t kLongestRun = 64;
constexpr std::array<std::size_t, 3> kGroups = {16, 64, 256};

// The image's length, as its header has it: four bytes from the eighth,
// the low one first (draw/font.h).
std::size_t LengthSaid(std::span<const std::uint8_t> image) {
  std::size_t length = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    length |= std::size_t{image[8 + i]} << (8 * i);
  }
  return length;
}

}  // namespace

PictureSizes& PictureSizes::operator+=(const PictureSizes& other) noexcept {
  asTheyAre += other.asTheyAre;
  runLength += other.runLength;
  deflate += other.deflate;
  best += other.best;
  groupsOf16 += other.groupsOf16;
  groupsOf64 += other.groupsOf64;
  groupsOf256 += other.groupsOf256;
  wholeFaces += other.wholeFaces;
  bestAsTheyAre += other.bestAsTheyAre;
  bestRunLength += other.bestRunLength;
  bestDeflate += other.bestDeflate;
  return *this;
}

std::vector<std::uint8_t> RunLength(const draw::Glyph& glyph) {
  std::vector<std::uint8_t> runs;
  const std::uint32_t pixels = std::uint32_t{glyph.width} * glyph.height;
  std::uint32_t level = 0;
  std::uint32_t length = 0;
  for (std::uint32_t pixel = 0; pixel < pixels; ++pixel) {
    const std::uint32_t ink = glyph.Ink(pixel % glyph.width, pixel / glyph.width);
    if (length > 0 && (ink != level || length == kLongestRun)) {
      runs.push_back(static_cast<std::uint8_t>((level << 6U) | (length - 1)));
      length = 0;
    }
    level = ink;
    ++length;
  }
  if (length > 0) {
    runs.push_back(static_cast<std::uint8_t>((level << 6U) | (length - 1)));
  }
  return runs;
}

std::vector<std::uint8_t> Deflated(std::span<const std::uint8_t> bytes) {
  z_stream stream{};
  // The most zlib does, in the window DEFLATE has, with no zlib header:
  // that is what a negative number of window bits asks for.
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  std::vector<std::uint8_t> out(deflateBound(&stream, static_cast<uLong>(bytes.size())));
  // zlib's interface is C's: it takes what it reads as bytes it may write.
  stream.next_in = const_cast<Bytef*>(bytes.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  out.resize(result == Z_STREAM_END ? stream.total_out : 0);
  deflateEnd(&stream);
  return out;
}

ImageSizes MeasureImage(std::span<const std::uint8_t> image) {
  ImageSizes sizes;
  draw::FontImage fonts;
  const draw::FontStatus status = fonts.Open(image);
  if (status != draw::FontStatus::kOk) {
    sizes.error = draw::FontStatusName(status);
    return sizes;
  }
  sizes.imageBytes = LengthSaid(image);
  std::map<std::uint16_t, SizeOfFaces> bySize;
  for (std::size_t number = 0; number < fonts.Faces(); ++number) {
    const draw::Face face = fonts.FaceAt(number);
    PictureSizes pictures;
    // The glyphs' bytes as they are, one after another: what is compressed
    // in groups, each group from where its first glyph begins.
    std::vector<std::uint8_t> together;
    std::vector<std::size_t> begins;
    for (std::uint32_t index = 0; index < face.Glyphs(); ++index) {
      draw::Glyph glyph;
      if (!face.GlyphAt(index, glyph)) {
        sizes.error = "glyph " + std::to_string(index) + " of face " + std::to_string(number) +
                      " has a picture that is not in the image";
        return sizes;
      }
      begins.push_back(together.size());
      together.insert(together.end(), glyph.picture.begin(), glyph.picture.end());
      const std::size_t asItIs = glyph.picture.size();
      const std::size_t runs = RunLength(glyph).size();
      const std::size_t deflated = asItIs == 0 ? 0 : Deflated(glyph.picture).size();
      pictures.asTheyAre += asItIs;
      pictures.runLength += runs;
      pictures.deflate += deflated;
      // The smallest; where two are the same, the one less work to draw.
      if (asItIs <= runs && asItIs <= deflated) {
        pictures.best += asItIs;
        ++pictures.bestAsTheyAre;
      } else if (runs <= deflated) {
        pictures.best += runs;
        ++pictures.bestRunLength;
      } else {
        pictures.best += deflated;
        ++pictures.bestDeflate;
      }
    }
    const std::span<const std::uint8_t> all(together);
    const auto grouped = [&](std::size_t glyphs) {
      std::size_t bytes = 0;
      for (std::size_t first = 0; first < begins.size(); first += glyphs) {
        const std::size_t from = begins[first];
        const std::size_t to = first + glyphs < begins.size() ? begins[first + glyphs] : together.size();
        bytes += to == from ? 0 : Deflated(all.subspan(from, to - from)).size();
      }
      return bytes;
    };
    pictures.groupsOf16 = grouped(kGroups[0]);
    pictures.groupsOf64 = grouped(kGroups[1]);
    pictures.groupsOf256 = grouped(kGroups[2]);
    pictures.wholeFaces = together.empty() ? 0 : Deflated(all).size();

    SizeOfFaces& ofSize = bySize[face.Size()];
    ofSize.size = face.Size();
    ++ofSize.faces;
    ofSize.glyphs += face.Glyphs();
    ofSize.pictures += pictures;
    ++sizes.all.faces;
    sizes.all.glyphs += face.Glyphs();
    sizes.all.pictures += pictures;
  }
  for (const auto& [size, ofSize] : bySize) {
    sizes.sizes.push_back(ofSize);
  }
  sizes.tableBytes =
      sizes.imageBytes >= sizes.all.pictures.asTheyAre ? sizes.imageBytes - sizes.all.pictures.asTheyAre : 0;
  sizes.ok = true;
  return sizes;
}

}  // namespace bn::font_tool
