#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "draw/png.h"
#include "png_builder.h"

// A second reading of a PNG file, the plain way: the whole file at once,
// its chunks looked at one after another, its data decompressed by zlib,
// its filters undone and its pixels turned to gray from their samples
// (png_builder.h). It follows the rules draw/png.h states, and shares none
// of the reader's code. tests/e2e/png_tool.py is the same reading in
// Python.

namespace bn::test {

struct PngReference {
  draw::PngStatus status = draw::PngStatus::kOk;
  bool headerKnown = false;
  PngPicture picture;  // Its size and kind: the samples are not kept.
  bool interlaced = false;
  std::vector<std::uint8_t> gray;  // The rows given, one after another.
  std::uint32_t rows = 0;
  std::uint32_t passed = 0;
  std::uint32_t bad = 0;
  // Whether where the reading stops is known for sure. It is not when a
  // chunk of the data goes wrong, or zlib refuses the data: the reader may
  // then stop sooner or later, by how far ahead its inflater has read.
  bool exact = true;
  // The rows came to more than it was asked to work out, and it left off:
  // the rows it has are right, and how the reading ends is not known.
  bool leftOff = false;
};

inline std::uint32_t ReferenceBe32(std::span<const std::uint8_t> bytes, std::size_t at) {
  return (std::uint32_t{bytes[at]} << 24U) | (std::uint32_t{bytes[at + 1]} << 16U) |
         (std::uint32_t{bytes[at + 2]} << 8U) | bytes[at + 3];
}

// What the reader should make of `file`, given `room` bytes for rows and a
// whole window. No more than `mostBytes` of decompressed rows are worked
// out.
inline PngReference ReadPngPlainly(std::span<const std::uint8_t> file, std::uint64_t room,
                                   std::size_t mostBytes = 67108864) {
  using draw::PngStatus;
  constexpr std::uint32_t kLongest = 0x7FFFFFFF;
  PngReference out;
  const auto fail = [&](PngStatus status) {
    out.status = status;
    return out;
  };
  constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (file.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), file.begin())) {
    return fail(PngStatus::kNotPng);
  }
  const auto isName = [&](std::size_t at) {
    return std::all_of(file.begin() + static_cast<std::ptrdiff_t>(at),
                       file.begin() + static_cast<std::ptrdiff_t>(at + 4),
                       [](std::uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); });
  };
  const auto named = [&](std::size_t at, const char* name) {
    return std::equal(name, name + 4, file.begin() + static_cast<std::ptrdiff_t>(at));
  };
  std::array<std::uint8_t, 256> palette{};  // As grays, alphas taken in.
  std::size_t entries = 0;
  bool seenPalette = false;
  bool seenAlphas = false;
  PngExtras extras;  // For the key only.
  std::size_t pos = 8;
  PngPicture& picture = out.picture;
  for (;;) {
    if (pos + 8 > file.size()) {
      return fail(PngStatus::kTruncated);
    }
    const std::uint32_t length = ReferenceBe32(file, pos);
    const std::size_t name = pos + 4;
    if (length > kLongest || !isName(name)) {
      return fail(out.headerKnown ? PngStatus::kBadChunk : PngStatus::kBadHeader);
    }
    enum class Kind : std::uint8_t { kHeader, kPalette, kAlphas, kOther } kind = Kind::kOther;
    if (!out.headerKnown) {
      if (!named(name, "IHDR") || length != 13) {
        return fail(PngStatus::kBadHeader);
      }
      kind = Kind::kHeader;
    } else if (named(name, "IHDR")) {
      return fail(PngStatus::kBadChunk);
    } else if (named(name, "IDAT")) {
      if (picture.colour == 3 && !seenPalette) {
        return fail(PngStatus::kNoPalette);
      }
      break;
    } else if (named(name, "IEND")) {
      return fail(PngStatus::kTruncated);
    } else if (named(name, "PLTE")) {
      if (picture.colour == 3) {
        if (seenPalette || length == 0 || length % 3 != 0 || length > 768) {
          return fail(PngStatus::kBadChunk);
        }
        kind = Kind::kPalette;
      }
    } else if (named(name, "tRNS")) {
      const bool usable =
          !seenAlphas && ((picture.colour == 0 && length == 2) || (picture.colour == 2 && length == 6) ||
                          (picture.colour == 3 && seenPalette && length <= entries));
      kind = usable ? Kind::kAlphas : Kind::kOther;
    } else if (file[name] >= 'A' && file[name] <= 'Z') {
      return fail(PngStatus::kUnknownChunk);
    }
    const std::size_t body = pos + 8;
    if (body + length + 4 > file.size()) {
      return fail(PngStatus::kTruncated);
    }
    const bool sound =
        static_cast<std::uint32_t>(crc32(0, file.data() + name, length + 4)) == ReferenceBe32(file, body + length);
    pos = body + length + 4;
    switch (kind) {
      case Kind::kHeader: {
        if (!sound) {
          return fail(PngStatus::kBadChecksum);
        }
        picture.width = ReferenceBe32(file, body);
        picture.height = ReferenceBe32(file, body + 4);
        picture.depth = file[body + 8];
        picture.colour = file[body + 9];
        const std::uint8_t c = picture.colour;
        const std::uint8_t d = picture.depth;
        const bool depthOk = (c == 0 && (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)) ||
                             (c == 3 && (d == 1 || d == 2 || d == 4 || d == 8)) ||
                             ((c == 2 || c == 4 || c == 6) && (d == 8 || d == 16));
        if (picture.width < 1 || picture.width > kLongest || picture.height < 1 || picture.height > kLongest ||
            !depthOk || file[body + 10] != 0 || file[body + 11] != 0 || file[body + 12] > 1) {
          return fail(PngStatus::kBadHeader);
        }
        out.headerKnown = true;
        out.interlaced = file[body + 12] == 1;
        if (out.interlaced) {
          return fail(PngStatus::kInterlaced);
        }
        const std::uint64_t stride = ((std::uint64_t{picture.width} * picture.Samples() * d) + 7) / 8;
        if (2 * std::max<std::uint64_t>(stride + 1, picture.width) > room) {
          return fail(PngStatus::kNoRoom);
        }
        break;
      }
      case Kind::kPalette:
        if (!sound) {
          return fail(PngStatus::kBadChecksum);
        }
        entries = length / 3;
        for (std::size_t entry = 0; entry < entries; ++entry) {
          palette[entry] =
              PlainGrayOf(file[body + (3 * entry)], file[body + (3 * entry) + 1], file[body + (3 * entry) + 2]);
        }
        seenPalette = true;
        break;
      case Kind::kAlphas:
        if (!sound) {
          ++out.passed;
          ++out.bad;
          break;
        }
        seenAlphas = true;
        if (picture.colour == 3) {
          for (std::size_t entry = 0; entry < length; ++entry) {
            palette[entry] = PlainOverWhite(palette[entry], file[body + entry]);
          }
        } else {
          extras.hasKey = true;
          for (std::size_t sample = 0; sample * 2 < length; ++sample) {
            extras.key[sample] = (std::uint32_t{file[body + (2 * sample)]} << 8U) | file[body + (2 * sample) + 1];
          }
        }
        break;
      case Kind::kOther:
        ++out.passed;
        out.bad += sound ? 0 : 1;
        break;
    }
  }

  // The data of the IDAT chunks that follow one another.
  std::vector<std::uint8_t> stream;
  bool chunkFailed = false;
  draw::PngStatus failure = PngStatus::kTruncated;
  std::uint32_t length = ReferenceBe32(file, pos);
  unsigned empty = length == 0 ? 1 : 0;
  for (;;) {
    const std::size_t start = pos + 8;
    const std::size_t end = std::min<std::size_t>(start + length, file.size());
    stream.insert(stream.end(), file.begin() + static_cast<std::ptrdiff_t>(start),
                  file.begin() + static_cast<std::ptrdiff_t>(end));
    if (start + length > file.size()) {
      break;
    }
    const std::size_t seam = start + length;
    const std::size_t seamBytes = std::min<std::size_t>(12, file.size() - seam);
    if (seamBytes >= 4 &&
        static_cast<std::uint32_t>(crc32(0, file.data() + pos + 4, length + 4)) != ReferenceBe32(file, seam)) {
      chunkFailed = true;
      failure = PngStatus::kBadChecksum;
      break;
    }
    if (seamBytes < 12 || !named(seam + 8, "IDAT")) {
      break;
    }
    pos = seam + 4;
    length = ReferenceBe32(file, pos);
    if (length > kLongest) {
      chunkFailed = true;
      failure = PngStatus::kBadChunk;
      break;
    }
    empty = length > 0 ? 0 : empty + 1;
    if (empty > draw::PngReader::kMaxEmptyChunksInARow) {
      chunkFailed = true;
      failure = PngStatus::kBadData;
      break;
    }
  }
  if (chunkFailed) {
    out.exact = false;
  }
  if (stream.size() < 2) {
    return fail(failure);
  }
  const unsigned method = stream[0];
  const unsigned flags = stream[1];
  if ((method & 15U) != 8 || (method >> 4U) > 7 || ((method << 8U) | flags) % 31 != 0 || (flags & 0x20U) != 0) {
    return fail(PngStatus::kBadData);
  }
  const std::size_t stride = picture.Stride();
  const std::uint64_t wanted = std::uint64_t{stride + 1} * picture.height;
  out.leftOff = wanted > mostBytes;
  std::vector<std::uint8_t> plain(static_cast<std::size_t>(std::min<std::uint64_t>(wanted, mostBytes)));
  z_stream z{};
  inflateInit2(&z, -15);
  std::vector<std::uint8_t> input(stream.begin() + 2, stream.end());
  z.next_in = input.data();
  z.avail_in = static_cast<uInt>(input.size());
  z.next_out = plain.data();
  z.avail_out = static_cast<uInt>(plain.size());
  const int result = inflate(&z, Z_FINISH);
  const std::size_t made = plain.size() - z.avail_out;
  inflateEnd(&z);
  // Left off, it stopped only for want of room: what it made is all there.
  out.leftOff = out.leftOff && made == plain.size();
  const bool zlibFailed = result == Z_DATA_ERROR || result == Z_MEM_ERROR || result == Z_NEED_DICT;
  if (zlibFailed) {
    out.exact = false;
  }
  // The rows that came out whole, filters undone.
  std::vector<std::uint8_t> above(stride, 0);
  std::vector<std::uint8_t> row(stride);
  const std::size_t back = picture.Back();
  for (std::uint32_t y = 0; y < picture.height && (y + 1) * (stride + 1) <= made; ++y) {
    const std::uint8_t* filtered = plain.data() + (y * (stride + 1));
    const std::uint8_t kind = filtered[0];
    if (kind > 4) {
      return fail(PngStatus::kBadData);
    }
    for (std::size_t at = 0; at < stride; ++at) {
      const int left = at >= back ? row[at - back] : 0;
      const int up = above[at];
      const int corner = at >= back ? above[at - back] : 0;
      int predicted = 0;
      if (kind == 1) {
        predicted = left;
      } else if (kind == 2) {
        predicted = up;
      } else if (kind == 3) {
        predicted = (left + up) / 2;
      } else if (kind == 4) {
        const int guess = left + up - corner;
        const int toLeft = std::abs(guess - left);
        const int toUp = std::abs(guess - up);
        const int toCorner = std::abs(guess - corner);
        predicted = toLeft <= toUp && toLeft <= toCorner ? left : (toUp <= toCorner ? up : corner);
      }
      row[at] = static_cast<std::uint8_t>(filtered[1 + at] + predicted);
    }
    PngPicture one = picture;
    one.height = 1;
    one.rows = row;
    std::vector<std::uint8_t> gray;
    if (picture.colour == 3) {
      for (std::uint32_t x = 0; x < picture.width; ++x) {
        gray.push_back(palette[one.Sample(0, x)]);
      }
    } else {
      gray = PlainGray(one, extras);
    }
    out.gray.insert(out.gray.end(), gray.begin(), gray.end());
    ++out.rows;
    above = row;
  }
  if (out.rows == picture.height) {
    out.status = PngStatus::kOk;
  } else if (zlibFailed) {
    out.status = PngStatus::kBadData;
  } else {
    out.status = failure;
  }
  return out;
}

}  // namespace bn::test
