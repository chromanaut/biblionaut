#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <span>
#include <vector>

// Making JPEG files in memory for tests, from the coefficients of each block
// rather than from pixels, and working out the gray a picture's pixels
// should come to from the same coefficients, the plain way: T.81's inverse
// transform in real numbers, a pixel at a time. So nothing is lost to
// encoding, and what the reader gives can be held to within a gray of what
// the picture is. Nothing here shares the reader's code. The Huffman tables
// are the standard's own, from its Annex K.

namespace bn::test {

using JpegBlock = std::array<std::int16_t, 64>;  // Quantized coefficients, row by row (v * 8 + u).

struct JpegComponent {
  std::uint8_t id = 1;
  std::uint8_t across = 1;  // Sampling.
  std::uint8_t down = 1;
  std::uint8_t table = 0;  // Of quantization.
};

// A picture as coefficients: each component's blocks on the grid of its
// units, padded to whole units, row by row.
struct JpegPicture {
  std::uint32_t width = 8;
  std::uint32_t height = 8;
  std::vector<JpegComponent> components = {JpegComponent{}};
  std::vector<std::vector<JpegBlock>> blocks;
  std::array<std::array<std::uint16_t, 64>, 4> steps{};  // Quantization, row by row.

  std::uint32_t MostAcross() const {
    std::uint32_t most = 1;
    for (const JpegComponent& c : components) {
      most = std::max<std::uint32_t>(most, c.across);
    }
    return most;
  }
  std::uint32_t MostDown() const {
    std::uint32_t most = 1;
    for (const JpegComponent& c : components) {
      most = std::max<std::uint32_t>(most, c.down);
    }
    return most;
  }
  std::uint32_t UnitsAcross() const { return (width + (8 * MostAcross()) - 1) / (8 * MostAcross()); }
  std::uint32_t UnitsDown() const { return (height + (8 * MostDown()) - 1) / (8 * MostDown()); }
  // Blocks across and down on component `i`'s grid.
  std::uint32_t BlocksAcross(std::size_t i) const { return UnitsAcross() * components[i].across; }
  std::uint32_t BlocksDown(std::size_t i) const { return UnitsDown() * components[i].down; }
  // The blocks a scan of component `i` alone covers: its own samples, in
  // whole blocks.
  std::uint32_t OwnBlocksAcross(std::size_t i) const {
    const std::uint32_t samples = ((width * components[i].across) + MostAcross() - 1) / MostAcross();
    return (samples + 7) / 8;
  }
  std::uint32_t OwnBlocksDown(std::size_t i) const {
    const std::uint32_t samples = ((height * components[i].down) + MostDown() - 1) / MostDown();
    return (samples + 7) / 8;
  }
  JpegBlock& Block(std::size_t i, std::uint32_t x, std::uint32_t y) { return blocks[i][(y * BlocksAcross(i)) + x]; }
  const JpegBlock& Block(std::size_t i, std::uint32_t x, std::uint32_t y) const {
    return blocks[i][(y * BlocksAcross(i)) + x];
  }

  // Every block of every component made, all 0, and every table a step of
  // `step`.
  void Make(std::uint16_t step = 1) {
    blocks.assign(components.size(), {});
    for (std::size_t i = 0; i < components.size(); ++i) {
      blocks[i].assign(std::size_t{BlocksAcross(i)} * BlocksDown(i), JpegBlock{});
    }
    for (auto& table : steps) {
      table.fill(step);
    }
  }
};

// How the file is written.
struct JpegOptions {
  std::uint8_t frame = 0xC0;  // The frame marker.
  std::uint8_t precision = 8;
  bool sixteenBitSteps = false;  // Quantization tables of 16 bits.
  std::uint16_t restartInterval = 0;
  // Whole segments, marker and all, put before the frame header, and
  // between the tables and the first scan.
  std::vector<std::vector<std::uint8_t>> before;
  std::vector<std::uint8_t> beforeScan;
  // Baseline: every component in one scan, or each in its own (and then the
  // colour's first, if asked).
  bool separateScans = false;
  bool colourFirst = false;
  // Progressive (frame 0xC2): the first scan holds the averages shifted down
  // by `shift`, of every component or of Y alone (and then the colour's
  // averages next); scans of other coefficients follow, which a reader of
  // the first scan passes over.
  std::uint8_t shift = 0;
  bool grayAverageAlone = false;
  bool colourAveragesFirst = false;
  // Fill bytes (0xFF) before each restart marker.
  std::size_t fill = 0;
  bool end = true;  // The end-of-image marker.
};

namespace jpeg_detail {

// T.81 Annex K.3: the standard tables for luminance and for chrominance.
struct Table {
  std::array<std::uint8_t, 16> counts;
  std::vector<std::uint8_t> values;
};

inline const Table& LuminanceAverages() {
  static const Table kTable{{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
  return kTable;
}
inline const Table& ChrominanceAverages() {
  static const Table kTable{{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
  return kTable;
}
inline const Table& LuminanceRest() {
  static const Table kTable{
      {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
      {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
       0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
       0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
       0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
       0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
       0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
       0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
       0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
       0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
  return kTable;
}
inline const Table& ChrominanceRest() {
  static const Table kTable{
      {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
      {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
       0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
       0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
       0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
       0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
       0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
       0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
       0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
       0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
  return kTable;
}

// The order coefficients are written in: [k] is where the k-th goes.
inline const std::array<std::uint8_t, 64>& Zigzag() {
  static const std::array<std::uint8_t, 64> kOrder = [] {
    std::array<std::uint8_t, 64> made{};
    std::size_t k = 0;
    for (std::size_t sum = 0; sum < 15; ++sum) {
      for (std::size_t i = 0; i <= sum; ++i) {
        // Up and to the right on odd diagonals, down and to the left on even.
        const std::size_t v = (sum % 2 == 0) ? sum - i : i;
        const std::size_t u = sum - v;
        if (u < 8 && v < 8) {
          made[k++] = static_cast<std::uint8_t>((v * 8) + u);
        }
      }
    }
    return made;
  }();
  return kOrder;
}

// Each value's code and its length, from a table's counts.
struct Codes {
  std::array<std::uint16_t, 256> code{};
  std::array<std::uint8_t, 256> length{};
};

inline Codes CodesOf(const Table& table) {
  Codes codes;
  std::uint32_t code = 0;
  std::size_t at = 0;
  for (std::size_t length = 1; length <= 16; ++length) {
    for (std::size_t i = 0; i < table.counts[length - 1]; ++i) {
      codes.code[table.values[at]] = static_cast<std::uint16_t>(code++);
      codes.length[table.values[at]] = static_cast<std::uint8_t>(length);
      ++at;
    }
    code <<= 1U;
  }
  return codes;
}

class Bits {
 public:
  explicit Bits(std::vector<std::uint8_t>& out) : out_(out) {}

  void Put(std::uint32_t value, unsigned count) {
    for (unsigned i = count; i > 0; --i) {
      byte_ = static_cast<std::uint8_t>((std::uint32_t{byte_} << 1U) | ((value >> (i - 1)) & 1U));
      if (++used_ == 8) {
        Byte();
      }
    }
  }
  // Ones to the end of the byte.
  void Pad() {
    while (used_ != 0) {
      Put(1, 1);
    }
  }

 private:
  void Byte() {
    out_.push_back(byte_);
    if (byte_ == 0xFF) {
      out_.push_back(0x00);
    }
    byte_ = 0;
    used_ = 0;
  }

  std::vector<std::uint8_t>& out_;
  std::uint8_t byte_ = 0;
  unsigned used_ = 0;
};

inline unsigned SizeOf(std::int32_t value) {
  unsigned size = 0;
  for (std::uint32_t magnitude = static_cast<std::uint32_t>(std::abs(value)); magnitude != 0; magnitude >>= 1U) {
    ++size;
  }
  return size;
}

// A value as its size's code and then its bits (T.81 F.1.2.1).
inline void PutValue(Bits& bits, const Codes& codes, std::uint8_t symbol, std::int32_t value, unsigned size) {
  bits.Put(codes.code[symbol], codes.length[symbol]);
  const std::int32_t written = value < 0 ? value - 1 : value;
  bits.Put(static_cast<std::uint32_t>(written) & ((1U << size) - 1U), size);
}

inline void Segment(std::vector<std::uint8_t>& out, std::uint8_t marker, const std::vector<std::uint8_t>& body) {
  out.push_back(0xFF);
  out.push_back(marker);
  const std::size_t length = body.size() + 2;
  out.push_back(static_cast<std::uint8_t>(length >> 8U));
  out.push_back(static_cast<std::uint8_t>(length));
  out.insert(out.end(), body.begin(), body.end());
}

inline void HuffmanSegment(std::vector<std::uint8_t>& out, std::uint8_t kindAndNumber, const Table& table) {
  std::vector<std::uint8_t> body = {kindAndNumber};
  body.insert(body.end(), table.counts.begin(), table.counts.end());
  body.insert(body.end(), table.values.begin(), table.values.end());
  Segment(out, 0xC4, body);
}

}  // namespace jpeg_detail

// `value` divided by 2 to the `shift`, rounded down: how T.81 (G.1.1.1.1)
// has a progressive first scan shift the averages it carries.
inline std::int32_t ShiftedDown(std::int32_t value, unsigned shift) {
  const auto by = static_cast<std::int32_t>(1U << shift);
  return value >= 0 ? value / by : -((-value + by - 1) / by);
}

// A segment, whole: its marker, its length and `body`.
inline std::vector<std::uint8_t> JpegSegment(std::uint8_t marker, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  jpeg_detail::Segment(out, marker, body);
  return out;
}

// The file. Component 0 uses the luminance tables (0), the rest the
// chrominance ones (1).
inline std::vector<std::uint8_t> JpegFile(const JpegPicture& picture, const JpegOptions& options = {}) {
  using namespace jpeg_detail;
  std::vector<std::uint8_t> out = {0xFF, 0xD8};
  for (const auto& segment : options.before) {
    out.insert(out.end(), segment.begin(), segment.end());
  }
  // The tables of quantization the components use.
  std::array<bool, 4> used{};
  for (const JpegComponent& c : picture.components) {
    used[c.table & 3U] = true;
  }
  for (std::size_t t = 0; t < 4; ++t) {
    if (!used[t]) {
      continue;
    }
    std::vector<std::uint8_t> body = {static_cast<std::uint8_t>((options.sixteenBitSteps ? 0x10U : 0U) | t)};
    for (std::size_t k = 0; k < 64; ++k) {
      const std::uint16_t step = picture.steps[t][Zigzag()[k]];
      if (options.sixteenBitSteps) {
        body.push_back(static_cast<std::uint8_t>(step >> 8U));
      }
      body.push_back(static_cast<std::uint8_t>(step));
    }
    Segment(out, 0xDB, body);
  }
  {
    std::vector<std::uint8_t> body = {options.precision,
                                      static_cast<std::uint8_t>(picture.height >> 8U),
                                      static_cast<std::uint8_t>(picture.height),
                                      static_cast<std::uint8_t>(picture.width >> 8U),
                                      static_cast<std::uint8_t>(picture.width),
                                      static_cast<std::uint8_t>(picture.components.size())};
    for (const JpegComponent& c : picture.components) {
      body.push_back(c.id);
      body.push_back(static_cast<std::uint8_t>((std::uint32_t{c.across} << 4U) | c.down));
      body.push_back(c.table);
    }
    Segment(out, options.frame, body);
  }
  HuffmanSegment(out, 0x00, LuminanceAverages());
  HuffmanSegment(out, 0x01, ChrominanceAverages());
  HuffmanSegment(out, 0x10, LuminanceRest());
  HuffmanSegment(out, 0x11, ChrominanceRest());
  if (options.restartInterval != 0) {
    Segment(
        out, 0xDD,
        {static_cast<std::uint8_t>(options.restartInterval >> 8U), static_cast<std::uint8_t>(options.restartInterval)});
  }
  out.insert(out.end(), options.beforeScan.begin(), options.beforeScan.end());

  const bool progressive = options.frame == 0xC2;
  const Codes dc[2] = {CodesOf(LuminanceAverages()), CodesOf(ChrominanceAverages())};
  const Codes ac[2] = {CodesOf(LuminanceRest()), CodesOf(ChrominanceRest())};

  // One scan of `which` components; averages alone if `averages`.
  const auto scan = [&](const std::vector<std::size_t>& which, bool averages) {
    std::vector<std::uint8_t> body = {static_cast<std::uint8_t>(which.size())};
    for (const std::size_t i : which) {
      const std::uint8_t tables = i == 0 ? 0x00 : 0x11;
      body.push_back(picture.components[i].id);
      body.push_back(averages && progressive ? static_cast<std::uint8_t>(tables & 0xF0U) : tables);
    }
    body.push_back(0);
    body.push_back(averages && progressive ? 0 : 63);
    body.push_back(progressive ? options.shift : 0);
    Segment(out, 0xDA, body);
    Bits bits(out);
    std::vector<std::int32_t> last(picture.components.size(), 0);
    const bool interleaved = which.size() > 1;
    const std::uint32_t across = interleaved ? picture.UnitsAcross() : picture.OwnBlocksAcross(which[0]);
    const std::uint32_t down = interleaved ? picture.UnitsDown() : picture.OwnBlocksDown(which[0]);
    std::uint32_t units = 0;
    std::uint32_t restarts = 0;
    for (std::uint32_t unitY = 0; unitY < down; ++unitY) {
      for (std::uint32_t unitX = 0; unitX < across; ++unitX) {
        if (options.restartInterval != 0 && units != 0 && units % options.restartInterval == 0) {
          bits.Pad();
          out.insert(out.end(), options.fill, 0xFF);
          out.push_back(0xFF);
          out.push_back(static_cast<std::uint8_t>(0xD0 + (restarts++ & 7U)));
          std::fill(last.begin(), last.end(), 0);
        }
        ++units;
        for (const std::size_t i : which) {
          const JpegComponent& c = picture.components[i];
          const std::uint32_t blocksAcross = interleaved ? c.across : 1;
          const std::uint32_t blocksDown = interleaved ? c.down : 1;
          const std::size_t kind = i == 0 ? 0 : 1;
          for (std::uint32_t by = 0; by < blocksDown; ++by) {
            for (std::uint32_t bx = 0; bx < blocksAcross; ++bx) {
              const JpegBlock& block = picture.Block(i, (unitX * blocksAcross) + bx, (unitY * blocksDown) + by);
              // The average, shifted as progressive scans have it.
              const std::int32_t average = progressive ? ShiftedDown(block[0], options.shift) : block[0];
              const std::int32_t difference = average - last[i];
              last[i] = average;
              const unsigned size = SizeOf(difference);
              PutValue(bits, dc[kind], static_cast<std::uint8_t>(size), difference, size);
              if (averages && progressive) {
                continue;
              }
              std::uint32_t run = 0;
              for (std::size_t k = 1; k < 64; ++k) {
                const std::int32_t value = block[Zigzag()[k]];
                if (value == 0) {
                  ++run;
                  continue;
                }
                for (; run >= 16; run -= 16) {
                  bits.Put(ac[kind].code[0xF0], ac[kind].length[0xF0]);
                }
                const unsigned valueSize = SizeOf(value);
                PutValue(bits, ac[kind], static_cast<std::uint8_t>((run << 4U) | valueSize), value, valueSize);
                run = 0;
              }
              if (run > 0) {
                bits.Put(ac[kind].code[0x00], ac[kind].length[0x00]);
              }
            }
          }
        }
      }
    }
    bits.Pad();
  };
  // A scan of coefficients past the averages, whose data a reader of the
  // first scan never looks at: bytes that make no marker.
  const auto passedOver = [&](std::size_t component, std::uint8_t first) {
    Segment(out, 0xDA, {1, picture.components[component].id, 0x00, first, 63, 0x00});
    for (std::size_t i = 0; i < 40; ++i) {
      const auto byte = static_cast<std::uint8_t>((i * 97) + first);
      out.push_back(byte);
      if (byte == 0xFF) {
        out.push_back(0x00);
      }
    }
  };

  std::vector<std::size_t> all;
  all.reserve(picture.components.size());
  for (std::size_t i = 0; i < picture.components.size(); ++i) {
    all.push_back(i);
  }
  if (progressive) {
    if (options.colourAveragesFirst && all.size() > 1) {
      scan({1, 2}, true);
      scan({0}, true);
    } else if (options.grayAverageAlone) {
      scan({0}, true);
      if (all.size() > 1) {
        scan({1, 2}, true);
      }
    } else {
      scan(all, true);
    }
    passedOver(0, 1);
    passedOver(0, 6);
  } else if (options.separateScans) {
    if (options.colourFirst) {
      for (std::size_t i = 1; i < all.size(); ++i) {
        scan({i}, false);
      }
      scan({0}, false);
    } else {
      for (const std::size_t i : all) {
        scan({i}, false);
      }
    }
  } else {
    scan(all, false);
  }
  if (options.end) {
    out.push_back(0xFF);
    out.push_back(0xD9);
  }
  return out;
}

// What a block's coefficients come to as pixels at `side` by `side` (8, 4,
// 2 or 1), in real numbers: T.81 A.3.3's inverse transform, and at fewer
// than 8 each pixel the average of those it stands for. `steps` are the
// quantization, row by row; `shift` how far the average was shifted down by
// a progressive first scan, which is all that is kept of such a picture.
inline std::vector<double> JpegBlockPixels(const JpegBlock& block, const std::array<std::uint16_t, 64>& steps,
                                           std::size_t side, unsigned shift = 0, bool averageOnly = false) {
  std::array<double, 64> whole{};
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      double sum = 0;
      for (std::size_t v = 0; v < 8; ++v) {
        for (std::size_t u = 0; u < 8; ++u) {
          double coefficient = block[(v * 8) + u];
          if (u == 0 && v == 0 && shift > 0) {
            coefficient = static_cast<double>(ShiftedDown(block[0], shift) * static_cast<std::int32_t>(1U << shift));
          } else if (averageOnly && (u != 0 || v != 0)) {
            coefficient = 0;
          }
          const double cu = u == 0 ? std::numbers::sqrt2 / 2 : 1.0;
          const double cv = v == 0 ? std::numbers::sqrt2 / 2 : 1.0;
          const auto across = static_cast<double>(((2 * x) + 1) * u);
          const auto down = static_cast<double>(((2 * y) + 1) * v);
          sum += cu * cv * coefficient * steps[(v * 8) + u] * std::cos(across * std::numbers::pi / 16) *
                 std::cos(down * std::numbers::pi / 16);
        }
      }
      whole[(y * 8) + x] = (sum / 4) + 128;
    }
  }
  const std::size_t cell = 8 / side;
  std::vector<double> pixels(side * side);
  for (std::size_t y = 0; y < side; ++y) {
    for (std::size_t x = 0; x < side; ++x) {
      double sum = 0;
      for (std::size_t j = 0; j < cell; ++j) {
        for (std::size_t i = 0; i < cell; ++i) {
          sum += whole[((y * cell) + j) * 8 + (x * cell) + i];
        }
      }
      pixels[(y * side) + x] = sum / static_cast<double>(cell * cell);
    }
  }
  return pixels;
}

// The gray a picture's pixels should come to, read `reduction` times
// smaller: Y's blocks, made pixels, the picture's own width and height
// taken from them. Not rounded or held to 0 to 255.
inline std::vector<double> JpegExpected(const JpegPicture& picture, std::uint32_t reduction, unsigned shift = 0) {
  const std::size_t side = 8 / reduction;
  const std::uint32_t width = (picture.width + reduction - 1) / reduction;
  const std::uint32_t height = (picture.height + reduction - 1) / reduction;
  const auto& steps = picture.steps[picture.components[0].table & 3U];
  std::vector<double> out(std::size_t{width} * height);
  for (std::uint32_t by = 0; by * side < height; ++by) {
    for (std::uint32_t bx = 0; bx * side < width; ++bx) {
      const std::vector<double> pixels = JpegBlockPixels(picture.Block(0, bx, by), steps, side, shift, shift > 0);
      for (std::size_t y = 0; y < side && (by * side) + y < height; ++y) {
        for (std::size_t x = 0; x < side && (bx * side) + x < width; ++x) {
          out[(((by * side) + y) * width) + (bx * side) + x] = pixels[(y * side) + x];
        }
      }
    }
  }
  return out;
}

}  // namespace bn::test
