#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Making PNG files in memory for tests, and working out the gray a
// picture's pixels should come to, the plain way: a pixel at a time, from
// its samples, with nothing in common with the reader but the two rules in
// draw/gray.h, which are written out again here.

namespace bn::test {

// A picture's samples as PNG packs them: each row's bytes, without the
// filter byte, one row after another.
struct PngPicture {
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::uint8_t depth = 8;
  std::uint8_t colour = 0;  // PNG's number for the kind of picture.
  std::vector<std::uint8_t> rows;

  std::size_t Samples() const {
    switch (colour) {
      case 2:
        return 3;
      case 4:
        return 2;
      case 6:
        return 4;
      default:
        return 1;
    }
  }
  std::size_t Stride() const { return ((std::size_t{width} * Samples() * depth) + 7) / 8; }
  // How far back the byte to the left is, for the filters.
  std::size_t Back() const { return std::max<std::size_t>(1, Samples() * depth / 8); }

  // Sample `index` of row `y`, counted in samples from the row's start, at
  // its full depth.
  std::uint32_t Sample(std::uint32_t y, std::size_t index) const {
    const std::uint8_t* row = rows.data() + (y * Stride());
    if (depth == 16) {
      return (std::uint32_t{row[index * 2]} << 8U) | row[(index * 2) + 1];
    }
    if (depth == 8) {
      return row[index];
    }
    const std::size_t bit = index * depth;
    return (std::uint32_t{row[bit / 8]} >> (8U - depth - (bit % 8U))) & ((1U << depth) - 1U);
  }
};

// What the tRNS chunk and the palette of a picture say, as the reader
// should take them.
struct PngExtras {
  std::vector<std::array<std::uint8_t, 3>> palette;  // Its colours.
  std::vector<std::uint8_t> alphas;                  // A palette's alphas, if tRNS gave them.
  bool hasKey = false;                               // tRNS gave a gray or colour for "none":
  std::array<std::uint32_t, 3> key{};                // this one.
};

inline std::uint8_t PlainGrayOf(std::uint32_t red, std::uint32_t green, std::uint32_t blue) {
  return static_cast<std::uint8_t>((77 * red + 150 * green + 29 * blue + 128) / 256);
}

// (gray * alpha + 255 * (255 - alpha)) / 255, to the nearest, a half up.
inline std::uint8_t PlainOverWhite(std::uint32_t gray, std::uint32_t alpha) {
  const std::uint32_t mixed = gray * alpha + 255 * (255 - alpha);
  return static_cast<std::uint8_t>((2 * mixed + 255) / 510);
}

// The gray every pixel of `picture` should be given: row after row.
inline std::vector<std::uint8_t> PlainGray(const PngPicture& picture, const PngExtras& extras) {
  std::vector<std::uint8_t> gray;
  gray.reserve(std::size_t{picture.width} * picture.height);
  const bool wide = picture.depth == 16;
  const auto eight = [&](std::uint32_t sample) -> std::uint32_t {
    if (wide) {
      return sample >> 8U;
    }
    if (picture.depth == 8) {
      return sample;
    }
    return sample * 255 / ((1U << picture.depth) - 1);
  };
  for (std::uint32_t y = 0; y < picture.height; ++y) {
    for (std::uint32_t x = 0; x < picture.width; ++x) {
      const std::size_t first = std::size_t{x} * picture.Samples();
      std::uint8_t value = 0;
      switch (picture.colour) {
        case 0: {
          const std::uint32_t sample = picture.Sample(y, first);
          value = extras.hasKey && sample == extras.key[0] ? 255 : static_cast<std::uint8_t>(eight(sample));
          break;
        }
        case 2: {
          const std::uint32_t red = picture.Sample(y, first);
          const std::uint32_t green = picture.Sample(y, first + 1);
          const std::uint32_t blue = picture.Sample(y, first + 2);
          const bool none = extras.hasKey && red == extras.key[0] && green == extras.key[1] && blue == extras.key[2];
          value = none ? 255 : PlainGrayOf(eight(red), eight(green), eight(blue));
          break;
        }
        case 3: {
          const std::uint32_t index = picture.Sample(y, first);
          if (index < extras.palette.size()) {
            const auto& colour = extras.palette[index];
            value = PlainGrayOf(colour[0], colour[1], colour[2]);
            if (index < extras.alphas.size()) {
              value = PlainOverWhite(value, extras.alphas[index]);
            }
          }
          break;
        }
        case 4:
          value = PlainOverWhite(eight(picture.Sample(y, first)), eight(picture.Sample(y, first + 1)));
          break;
        case 6:
          value = PlainOverWhite(PlainGrayOf(eight(picture.Sample(y, first)), eight(picture.Sample(y, first + 1)),
                                             eight(picture.Sample(y, first + 2))),
                                 eight(picture.Sample(y, first + 3)));
          break;
        default:
          break;
      }
      gray.push_back(value);
    }
  }
  return gray;
}

// The rows with a filter byte in front of each, filtered by it: row y by
// filters[y % filters.size()], 0 to 4.
inline std::vector<std::uint8_t> FilterPngRows(const PngPicture& picture, std::span<const std::uint8_t> filters) {
  const std::size_t stride = picture.Stride();
  const std::size_t back = picture.Back();
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> zeros(stride, 0);
  for (std::uint32_t y = 0; y < picture.height; ++y) {
    const std::uint8_t* row = picture.rows.data() + (y * stride);
    const std::uint8_t* above = y == 0 ? zeros.data() : picture.rows.data() + ((y - 1) * stride);
    const std::uint8_t filter = filters[y % filters.size()];
    out.push_back(filter);
    for (std::size_t at = 0; at < stride; ++at) {
      const int left = at >= back ? row[at - back] : 0;
      const int up = above[at];
      const int corner = at >= back ? above[at - back] : 0;
      int predicted = 0;
      switch (filter) {
        case 1:
          predicted = left;
          break;
        case 2:
          predicted = up;
          break;
        case 3:
          predicted = (left + up) / 2;
          break;
        case 4: {
          const int guess = left + up - corner;
          const int toLeft = guess > left ? guess - left : left - guess;
          const int toUp = guess > up ? guess - up : up - guess;
          const int toCorner = guess > corner ? guess - corner : corner - guess;
          predicted = toLeft <= toUp && toLeft <= toCorner ? left : (toUp <= toCorner ? up : corner);
          break;
        }
        default:
          break;
      }
      out.push_back(static_cast<std::uint8_t>(row[at] - predicted));
    }
  }
  return out;
}

inline void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24U));
  out.push_back(static_cast<std::uint8_t>(value >> 16U));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

// `data` as a zlib stream, as PNG's compressed data is.
inline std::vector<std::uint8_t> ZlibStream(std::span<const std::uint8_t> data, int level = 9) {
  uLongf size = compressBound(static_cast<uLong>(data.size()));
  std::vector<std::uint8_t> out(size);
  compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), level);
  out.resize(size);
  return out;
}

// What a header chunk holds.
inline std::vector<std::uint8_t> PngHeaderBytes(std::uint32_t width, std::uint32_t height, std::uint8_t depth,
                                                std::uint8_t colour, std::uint8_t interlace = 0) {
  std::vector<std::uint8_t> bytes;
  PutBe32(bytes, width);
  PutBe32(bytes, height);
  bytes.insert(bytes.end(), {depth, colour, 0, 0, interlace});
  return bytes;
}

// A PNG file, a chunk at a time.
class PngFile {
 public:
  static constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

  PngFile() : bytes_(kSignature.begin(), kSignature.end()) {}

  // A chunk named `name` holding `data`, with its checksum right, or
  // wrong if `spoil`.
  PngFile& Add(std::string_view name, std::span<const std::uint8_t> data, bool spoil = false) {
    PutBe32(bytes_, static_cast<std::uint32_t>(data.size()));
    const std::size_t named = bytes_.size();
    bytes_.insert(bytes_.end(), name.begin(), name.end());
    bytes_.insert(bytes_.end(), data.begin(), data.end());
    const std::uint32_t crc =
        static_cast<std::uint32_t>(crc32(0, bytes_.data() + named, static_cast<uInt>(bytes_.size() - named)));
    PutBe32(bytes_, spoil ? crc ^ 1U : crc);
    return *this;
  }
  PngFile& Add(std::string_view name, std::initializer_list<std::uint8_t> data, bool spoil = false) {
    return Add(name, std::span<const std::uint8_t>(data.begin(), data.size()), spoil);
  }

  PngFile& Header(std::uint32_t width, std::uint32_t height, std::uint8_t depth, std::uint8_t colour,
                  std::uint8_t interlace = 0) {
    const std::vector<std::uint8_t> bytes = PngHeaderBytes(width, height, depth, colour, interlace);
    return Add("IHDR", bytes);
  }

  // `stream` in IDAT chunks of `piece` bytes, with `empty` chunks holding
  // nothing after each but the last.
  PngFile& Data(std::span<const std::uint8_t> stream, std::size_t piece = 65536, unsigned empty = 0) {
    piece = std::max<std::size_t>(piece, 1);
    std::size_t at = 0;
    do {
      const std::size_t count = std::min(piece, stream.size() - at);
      Add("IDAT", stream.subspan(at, count));
      at += count;
      if (at < stream.size()) {
        for (unsigned i = 0; i < empty; ++i) {
          Add("IDAT", std::span<const std::uint8_t>());
        }
      }
    } while (at < stream.size());
    return *this;
  }

  PngFile& End() { return Add("IEND", std::span<const std::uint8_t>()); }

  const std::vector<std::uint8_t>& Bytes() const { return bytes_; }
  std::vector<std::uint8_t>& Bytes() { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

// A whole sound file for `picture`, its rows filtered by `filters`, and
// with `before` added ahead of its data: a palette, transparency, anything.
inline std::vector<std::uint8_t> PngFor(
    const PngPicture& picture, std::span<const std::uint8_t> filters,
    const std::vector<std::pair<std::string_view, std::vector<std::uint8_t>>>& before = {}, std::size_t piece = 65536) {
  PngFile file;
  file.Header(picture.width, picture.height, picture.depth, picture.colour);
  for (const auto& [name, data] : before) {
    file.Add(name, data);
  }
  const std::vector<std::uint8_t> filtered = FilterPngRows(picture, filters);
  file.Data(ZlibStream(filtered), piece);
  file.End();
  return file.Bytes();
}

}  // namespace bn::test
