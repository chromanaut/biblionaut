#include "draw/jpeg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "draw/fixed.h"

namespace bn::draw {

namespace {

// Markers, by the byte after their 0xFF.
constexpr std::uint8_t kStartOfImage = 0xD8;
constexpr std::uint8_t kEndOfImage = 0xD9;
constexpr std::uint8_t kFirstRestart = 0xD0;
constexpr std::uint8_t kLastRestart = 0xD7;
constexpr std::uint8_t kTemporary = 0x01;
constexpr std::uint8_t kHuffmanTables = 0xC4;
constexpr std::uint8_t kArithmeticConditioning = 0xCC;
constexpr std::uint8_t kExtension = 0xC8;
constexpr std::uint8_t kQuantizationTables = 0xDB;
constexpr std::uint8_t kRestartInterval = 0xDD;
constexpr std::uint8_t kStartOfScan = 0xDA;
constexpr std::uint8_t kAdobe = 0xEE;

// The frame headers: 0xC0 to 0xCF but for the three above that share their
// range. Baseline, extended and progressive, with Huffman codes, are read.
bool IsFrame(std::uint8_t marker) noexcept {
  return marker >= 0xC0 && marker <= 0xCF && marker != kHuffmanTables && marker != kExtension &&
         marker != kArithmeticConditioning;
}
bool IsProgressiveFrame(std::uint8_t marker) noexcept {
  return marker == 0xC2 || marker == 0xC6 || marker == 0xCA || marker == 0xCE;
}

// Where each coefficient goes in its block, in the order they are coded.
constexpr std::array<std::uint8_t, 64> kNatural = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// The inverse transform, one direction at a time: [u][x] is what frequency u
// gives pixel x, C(u)/2 * cos((2x + 1)u pi / 16), C(0) being 1/sqrt(2) and
// C(u) 1 otherwise, times 4096 and rounded. Used both ways, it is T.81's
// inverse transform (A.3.3).
constexpr std::array<std::array<std::int32_t, 8>, 8> kWhole = {{
    {1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448},
    {2009, 1703, 1138, 400, -400, -1138, -1703, -2009},
    {1892, 784, -784, -1892, -1892, -784, 784, 1892},
    {1703, -400, -2009, -1138, 1138, 2009, 400, -1703},
    {1448, -1448, -1448, 1448, 1448, -1448, -1448, 1448},
    {1138, -2009, 400, 1703, -1703, -400, 2009, -1138},
    {784, -1892, 1892, -784, -784, 1892, -1892, 784},
    {400, -1138, 1703, -2009, 2009, -1703, 1138, -400},
}};
// The same made smaller: [u][X] is the average of what frequency u gives the
// 2 or 4 pixels that pixel X stands for (worked out from the cosines, not
// from the rounded table above). So each pixel read at a half or a quarter
// is the average of those the block would have at its whole size.
constexpr std::array<std::array<std::int32_t, 4>, 8> kHalf = {{
    {1448, 1448, 1448, 1448},
    {1856, 769, -769, -1856},
    {1338, -1338, -1338, 1338},
    {652, -1573, 1573, -652},
    {0, 0, 0, 0},
    {-435, 1051, -1051, 435},
    {-554, 554, 554, -554},
    {-369, -153, 153, 369},
}};
constexpr std::array<std::array<std::int32_t, 2>, 8> kQuarter = {{
    {1448, 1448},
    {1312, -1312},
    {0, 0},
    {-461, 461},
    {0, 0},
    {308, -308},
    {0, 0},
    {-261, 261},
}};

// A coefficient made whole by its quantization step is held to this: far
// more than any picture's (1,024 and half a step), and small enough that
// the transform's sums cannot overflow.
constexpr std::int32_t kMostCoefficient = 16383;
// The transform's numbers between its two directions, with 3 bits below
// the point: past what any picture gives (about 37,000).
constexpr std::int32_t kMostBetween = 65535;
// An average as a scan carries it, before its quantization step.
constexpr std::int32_t kMostAverage = 32767;

std::int32_t Clamp(std::int64_t value, std::int32_t most) noexcept {
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, -most, most));
}

std::uint8_t ToPixel(std::int32_t value) noexcept {
  return static_cast<std::uint8_t>(std::clamp<std::int32_t>(value + 128, 0, 255));
}

// The block's coefficients turned into N by N pixels, down each column of
// frequencies and then across each row, rows `stride` apart. Every
// coefficient right of `lastAcross` or below `lastDown` is 0.
template <std::size_t N>
void Transform(const std::array<std::int32_t, 64>& block, std::array<std::int32_t, 64>& work,
               const std::array<std::array<std::int32_t, N>, 8>& table, std::size_t lastAcross, std::size_t lastDown,
               std::uint8_t* out, std::size_t stride) noexcept {
  for (std::size_t u = 0; u <= lastAcross; ++u) {
    for (std::size_t y = 0; y < N; ++y) {
      std::int32_t sum = 0;
      for (std::size_t v = 0; v <= lastDown; ++v) {
        sum += table[v][y] * block[(v * 8) + u];
      }
      // Three bits kept below the point.
      work[(y * 8) + u] = std::clamp<std::int32_t>(ShiftDown(sum + 256, 9), -kMostBetween, kMostBetween);
    }
  }
  for (std::size_t y = 0; y < N; ++y) {
    std::uint8_t* row = out + (y * stride);
    for (std::size_t x = 0; x < N; ++x) {
      std::int32_t sum = 0;
      for (std::size_t u = 0; u <= lastAcross; ++u) {
        sum += table[u][x] * work[(y * 8) + u];
      }
      row[x] = ToPixel(ShiftDown(sum + 16384, 15));
    }
  }
}

}  // namespace

const char* JpegStatusName(JpegStatus status) noexcept {
  switch (status) {
    case JpegStatus::kOk:
      return "ok";
    case JpegStatus::kNotJpeg:
      return "not jpeg";
    case JpegStatus::kBadMarker:
      return "bad marker";
    case JpegStatus::kBadHeader:
      return "bad header";
    case JpegStatus::kBadTable:
      return "bad table";
    case JpegStatus::kBadScan:
      return "bad scan";
    case JpegStatus::kUnsupported:
      return "unsupported";
    case JpegStatus::kBadData:
      return "bad data";
    case JpegStatus::kTruncated:
      return "truncated";
    case JpegStatus::kNoRoom:
      return "no room";
    case JpegStatus::kSourceFailed:
      return "source failed";
  }
  return "unknown";
}

const char* JpegKindName(const JpegHeader& header) noexcept {
  struct Kind {
    std::uint8_t across;
    std::uint8_t down;
    const char* name;
    const char* progressive;
  };
  static constexpr std::array<Kind, 5> kKinds = {{
      {1, 1, "4:4:4", "progressive 4:4:4"},
      {2, 1, "4:2:2", "progressive 4:2:2"},
      {2, 2, "4:2:0", "progressive 4:2:0"},
      {1, 2, "4:4:0", "progressive 4:4:0"},
      {4, 1, "4:1:1", "progressive 4:1:1"},
  }};
  if (header.components == 1) {
    return header.progressive ? "progressive gray" : "gray";
  }
  if (header.components != 3) {
    return header.components == 4 ? "cmyk" : "other";
  }
  if (header.colourAcross == 1 && header.colourDown == 1) {
    for (const Kind& kind : kKinds) {
      if (kind.across == header.grayAcross && kind.down == header.grayDown) {
        return header.progressive ? kind.progressive : kind.name;
      }
    }
  }
  return header.progressive ? "progressive ycbcr" : "ycbcr";
}

JpegReduction JpegReader::ReductionFor(const JpegHeader& header, std::uint32_t shownWidth,
                                       std::uint32_t shownHeight) noexcept {
  if (header.progressive) {
    return JpegReduction::kEighth;
  }
  for (const JpegReduction reduction : {JpegReduction::kEighth, JpegReduction::kQuarter, JpegReduction::kHalf}) {
    if (WidthAt(header, reduction) >= shownWidth && HeightAt(header, reduction) >= shownHeight) {
      return reduction;
    }
  }
  return JpegReduction::kNone;
}

JpegReduction JpegReader::ReductionTaken(const JpegHeader& header, JpegReduction asked) noexcept {
  if (header.progressive) {
    return JpegReduction::kEighth;
  }
  switch (asked) {
    case JpegReduction::kNone:
    case JpegReduction::kHalf:
    case JpegReduction::kQuarter:
    case JpegReduction::kEighth:
      return asked;
  }
  return JpegReduction::kNone;
}

std::uint32_t JpegReader::WidthAt(const JpegHeader& header, JpegReduction asked) noexcept {
  const auto by = static_cast<std::uint32_t>(ReductionTaken(header, asked));
  return (header.width + by - 1) / by;
}

std::uint32_t JpegReader::HeightAt(const JpegHeader& header, JpegReduction asked) noexcept {
  const auto by = static_cast<std::uint32_t>(ReductionTaken(header, asked));
  return (header.height + by - 1) / by;
}

std::uint64_t JpegReader::RowBytesFor(const JpegHeader& header, JpegReduction asked) noexcept {
  if (header.width == 0 || header.height == 0 || header.grayAcross == 0 || header.grayDown == 0) {
    return 0;
  }
  const std::uint64_t side = 8 / static_cast<std::uint64_t>(ReductionTaken(header, asked));
  if (header.components == 1) {
    // One component is always a scan by itself, a block to a unit.
    return ((header.width + 7) / 8) * side * side;
  }
  if (header.components != 3 || header.colourAcross > header.grayAcross || header.colourDown > header.grayDown) {
    return 0;
  }
  // Units of blocks across, each Y's blocks across, and Y's blocks down. A
  // scan of Y alone needs no more.
  const std::uint64_t across = header.grayAcross;
  const std::uint64_t units = (header.width + (8 * across) - 1) / (8 * across);
  return units * across * side * header.grayDown * side;
}

JpegReader::JpegReader(std::span<std::uint8_t> rows) noexcept : rowMemory_(rows) {}

void JpegReader::Begin(core::ByteSource& source, RowSink* sink, JpegReduction reduction) noexcept {
  source_ = &source;
  sink_ = sink;
  status_ = JpegStatus::kOk;
  stage_ = Stage::kStart;
  asked_ = reduction;
  reduction_ = JpegReduction::kNone;
  inputAt_ = 0;
  inputEnd_ = 0;
  ended_ = false;
  failed_ = false;
  sawFF_ = false;
  header_ = JpegHeader{};
  headerKnown_ = false;
  frameSeen_ = false;
  frameMarker_ = 0;
  adobeTransform_ = -1;
  restartInterval_ = 0;
  componentCount_ = 0;
  components_ = {};
  for (Huffman& table : huffman_) {
    table.defined = false;
  }
  quantizationDefined_ = {};
  segment_ = Segment::kPass;
  segmentLeft_ = 0;
  scanCount_ = 0;
  successive_ = 0;
  interleaved_ = false;
  ResetBits();
  blockSide_ = 8;
  bufferWidth_ = 0;
  rowsPerMcu_ = 0;
  outWidth_ = 0;
  outHeight_ = 0;
  rowsGiven_ = 0;
  rowsMade_ = 0;
  segmentsPassedOver_ = 0;
  scansPassedOver_ = 0;
  junkBytes_ = 0;
}

JpegStatus JpegReader::Read(core::ByteSource& source, RowSink* sink, JpegReduction reduction) noexcept {
  Begin(source, sink, reduction);
  while (Step()) {
  }
  return status_;
}

bool JpegReader::Step() noexcept {
  switch (stage_) {
    case Stage::kStart:
      return ReadStart();
    case Stage::kMarker:
      return FindMarker();
    case Stage::kSegment:
      return ReadSegment();
    case Stage::kSkipScan:
      return SkipScan();
    case Stage::kDecode:
      return Decode();
    case Stage::kGive:
      return Give();
    case Stage::kDone:
      return false;
  }
  return false;
}

bool JpegReader::Fail(JpegStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kDone;
  return false;
}

bool JpegReader::Finish() noexcept {
  stage_ = Stage::kDone;
  return false;
}

// ---- Bytes ----

bool JpegReader::Byte(std::uint8_t& out) noexcept {
  if (inputAt_ == inputEnd_) {
    if (ended_ || failed_) {
      return false;
    }
    const std::ptrdiff_t read = source_->Read(input_);
    if (read < 0) {
      failed_ = true;
      return false;
    }
    if (read == 0) {
      ended_ = true;
      return false;
    }
    inputAt_ = 0;
    inputEnd_ = static_cast<std::size_t>(read);
  }
  out = input_[inputAt_++];
  return true;
}

bool JpegReader::Fill(std::span<std::uint8_t> dst) noexcept {
  for (std::uint8_t& byte : dst) {
    if (!Byte(byte)) {
      return Fail(failed_ ? JpegStatus::kSourceFailed : JpegStatus::kTruncated);
    }
  }
  segmentLeft_ -= std::min(segmentLeft_, dst.size());
  return true;
}

// ---- Markers and segments ----

bool JpegReader::ReadStart() noexcept {
  std::array<std::uint8_t, 2> start{};
  for (std::uint8_t& byte : start) {
    if (!Byte(byte)) {
      return Fail(failed_ ? JpegStatus::kSourceFailed : JpegStatus::kNotJpeg);
    }
  }
  if (start[0] != 0xFF || start[1] != kStartOfImage) {
    return Fail(JpegStatus::kNotJpeg);
  }
  stage_ = Stage::kMarker;
  return true;
}

bool JpegReader::FindMarker() noexcept {
  for (std::size_t count = 0; count < kSegmentBytes; ++count) {
    std::uint8_t byte = 0;
    if (!Byte(byte)) {
      if (failed_) {
        return Fail(JpegStatus::kSourceFailed);
      }
      return Fail(frameSeen_ ? JpegStatus::kTruncated : JpegStatus::kBadHeader);
    }
    if (!sawFF_) {
      if (byte == 0xFF) {
        sawFF_ = true;
      } else {
        ++junkBytes_;
      }
      continue;
    }
    if (byte == 0xFF) {
      continue;  // A fill byte.
    }
    sawFF_ = false;
    if (byte == 0x00) {
      junkBytes_ += 2;
      continue;
    }
    return TakeMarker(byte);
  }
  return true;
}

bool JpegReader::SkipScan() noexcept {
  for (std::size_t count = 0; count < kSegmentBytes; ++count) {
    std::uint8_t byte = 0;
    if (!Byte(byte)) {
      return Fail(failed_ ? JpegStatus::kSourceFailed : JpegStatus::kTruncated);
    }
    if (!sawFF_) {
      sawFF_ = byte == 0xFF;
      continue;
    }
    if (byte == 0xFF) {
      continue;
    }
    sawFF_ = false;
    // A 0xFF of the data, and restart markers, are the scan's own.
    if (byte == 0x00 || (byte >= kFirstRestart && byte <= kLastRestart)) {
      continue;
    }
    return TakeMarker(byte);
  }
  return true;
}

bool JpegReader::TakeMarker(std::uint8_t marker) noexcept {
  if (marker == kStartOfImage) {
    return Fail(JpegStatus::kBadMarker);
  }
  if (marker == kEndOfImage) {
    // Before the scan that has the picture's rows.
    return Fail(frameSeen_ ? JpegStatus::kTruncated : JpegStatus::kBadHeader);
  }
  if (marker == kTemporary || (marker >= kFirstRestart && marker <= kLastRestart)) {
    ++segmentsPassedOver_;
    stage_ = Stage::kMarker;
    return true;
  }
  std::array<std::uint8_t, 2> length{};
  for (std::uint8_t& byte : length) {
    if (!Byte(byte)) {
      return Fail(failed_ ? JpegStatus::kSourceFailed : JpegStatus::kTruncated);
    }
  }
  const std::size_t whole = (std::size_t{length[0]} << 8U) | length[1];
  if (whole < 2) {
    return Fail(JpegStatus::kBadMarker);
  }
  segmentLeft_ = whole - 2;
  stage_ = Stage::kSegment;
  if (IsFrame(marker)) {
    segment_ = Segment::kFrame;
    frameMarker_ = marker;
  } else if (marker == kHuffmanTables) {
    segment_ = Segment::kHuffman;
  } else if (marker == kQuantizationTables) {
    segment_ = Segment::kQuantization;
  } else if (marker == kRestartInterval) {
    segment_ = Segment::kRestart;
  } else if (marker == kStartOfScan) {
    if (!frameSeen_) {
      return Fail(JpegStatus::kBadHeader);
    }
    segment_ = Segment::kScan;
  } else if (marker == kAdobe && !frameSeen_) {
    segment_ = Segment::kAdobe;
  } else {
    segment_ = Segment::kPass;
    ++segmentsPassedOver_;
  }
  return true;
}

bool JpegReader::ReadSegment() noexcept {
  switch (segment_) {
    case Segment::kPass: {
      const std::size_t count = std::min(segmentLeft_, kSegmentBytes);
      for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t byte = 0;
        if (!Byte(byte)) {
          return Fail(failed_ ? JpegStatus::kSourceFailed : JpegStatus::kTruncated);
        }
      }
      segmentLeft_ -= count;
      if (segmentLeft_ == 0) {
        stage_ = Stage::kMarker;
      }
      return true;
    }
    case Segment::kHuffman:
      return ReadHuffman();
    case Segment::kQuantization:
      return ReadQuantization();
    case Segment::kFrame:
      return ReadFrame(frameMarker_, segmentLeft_);
    case Segment::kScan:
      return ReadScan(segmentLeft_);
    case Segment::kRestart:
      return ReadRestartInterval(segmentLeft_);
    case Segment::kAdobe:
      return ReadAdobe(segmentLeft_);
  }
  return Fail(JpegStatus::kBadMarker);
}

bool JpegReader::ReadAdobe(std::size_t length) noexcept {
  // "Adobe", its version and two words of flags, and the colour transform:
  // 0 for RGB (or CMYK), 1 for YCbCr, 2 for YCCK.
  std::array<std::uint8_t, 12> body{};
  segment_ = Segment::kPass;
  if (length < body.size()) {
    return true;
  }
  if (!Fill(body)) {
    return false;
  }
  if (body[0] == 'A' && body[1] == 'd' && body[2] == 'o' && body[3] == 'b' && body[4] == 'e') {
    adobeTransform_ = body[11];
  }
  if (segmentLeft_ == 0) {
    stage_ = Stage::kMarker;
  }
  return true;
}

bool JpegReader::ReadRestartInterval(std::size_t length) noexcept {
  std::array<std::uint8_t, 2> body{};
  if (length != body.size()) {
    return Fail(JpegStatus::kBadMarker);
  }
  if (!Fill(body)) {
    return false;
  }
  restartInterval_ = static_cast<std::uint16_t>((std::uint32_t{body[0]} << 8U) | body[1]);
  stage_ = Stage::kMarker;
  return true;
}

bool JpegReader::ReadHuffman() noexcept {
  if (segmentLeft_ == 0) {
    stage_ = Stage::kMarker;
    return true;
  }
  std::array<std::uint8_t, 17> head{};
  if (segmentLeft_ < head.size()) {
    return Fail(JpegStatus::kBadTable);
  }
  if (!Fill(head)) {
    return false;
  }
  const unsigned kind = head[0] >> 4U;
  const unsigned number = head[0] & 15U;
  if (kind > 1 || number > 3) {
    return Fail(JpegStatus::kBadTable);
  }
  std::size_t total = 0;
  for (std::size_t length = 1; length <= 16; ++length) {
    total += head[length];
  }
  if (total > 256 || total > segmentLeft_) {
    return Fail(JpegStatus::kBadTable);
  }
  Huffman& table = huffman_[(kind * 4) + number];
  table = Huffman{};
  if (!Fill(std::span(table.values).first(total))) {
    return false;
  }
  // The codes, shortest first and in order within a length (T.81 C.2), none
  // of them all ones.
  std::uint32_t code = 0;
  std::uint32_t at = 0;
  for (std::size_t length = 1; length <= 16; ++length) {
    const std::uint32_t count = head[length];
    table.largest[length] = -1;
    if (count > 0) {
      table.offset[length] = static_cast<std::int32_t>(at) - static_cast<std::int32_t>(code);
      if (length <= 8) {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::size_t first = std::size_t{code + i} << (8 - length);
          const std::size_t span = std::size_t{1} << (8 - length);
          for (std::size_t s = 0; s < span && first + s < 256; ++s) {
            table.fastLength[first + s] = static_cast<std::uint8_t>(length);
            table.fastValue[first + s] = table.values[std::size_t{at} + i];
          }
        }
      }
      code += count;
      at += count;
      if (code >= (1U << length)) {
        return Fail(JpegStatus::kBadTable);
      }
      table.largest[length] = static_cast<std::int32_t>(code - 1);
    }
    code <<= 1U;
  }
  table.defined = true;
  return true;
}

bool JpegReader::ReadQuantization() noexcept {
  if (segmentLeft_ == 0) {
    stage_ = Stage::kMarker;
    return true;
  }
  std::array<std::uint8_t, 1> head{};
  if (!Fill(head)) {
    return false;
  }
  const unsigned precision = head[0] >> 4U;
  const unsigned number = head[0] & 15U;
  if (precision > 1 || number > 3) {
    return Fail(JpegStatus::kBadTable);
  }
  const std::size_t size = precision == 0 ? 64 : 128;
  if (segmentLeft_ < size) {
    return Fail(JpegStatus::kBadTable);
  }
  std::array<std::uint8_t, 128> body{};
  if (!Fill(std::span(body).first(size))) {
    return false;
  }
  for (std::size_t k = 0; k < 64; ++k) {
    quantization_[number][k] =
        precision == 0 ? body[k] : static_cast<std::uint16_t>((std::uint32_t{body[2 * k]} << 8U) | body[(2 * k) + 1]);
  }
  quantizationDefined_[number] = true;
  return true;
}

bool JpegReader::ReadFrame(std::uint8_t marker, std::size_t length) noexcept {
  if (frameSeen_) {
    return Fail(JpegStatus::kBadHeader);
  }
  frameSeen_ = true;
  std::array<std::uint8_t, 6> head{};
  if (length < head.size()) {
    return Fail(JpegStatus::kBadHeader);
  }
  if (!Fill(head)) {
    return false;
  }
  const unsigned precision = head[0];
  const std::uint32_t height = (std::uint32_t{head[1]} << 8U) | head[2];
  const std::uint32_t width = (std::uint32_t{head[3]} << 8U) | head[4];
  const unsigned count = head[5];
  if (count == 0 || length != head.size() + (3 * std::size_t{count}) || width == 0) {
    return Fail(JpegStatus::kBadHeader);
  }
  for (unsigned i = 0; i < count; ++i) {
    std::array<std::uint8_t, 3> entry{};
    if (!Fill(entry)) {
      return false;
    }
    const unsigned across = entry[1] >> 4U;
    const unsigned down = entry[1] & 15U;
    if (across < 1 || across > 4 || down < 1 || down > 4 || entry[2] > 3) {
      return Fail(JpegStatus::kBadHeader);
    }
    if (i < kMostComponents) {
      for (unsigned j = 0; j < i; ++j) {
        if (components_[j].id == entry[0]) {
          return Fail(JpegStatus::kBadHeader);
        }
      }
      components_[i] =
          Component{entry[0], static_cast<std::uint8_t>(across), static_cast<std::uint8_t>(down), entry[2]};
    }
  }
  if (precision != 8 && precision != 12 && precision != 16) {
    return Fail(JpegStatus::kBadHeader);
  }
  componentCount_ = static_cast<std::uint8_t>(std::min<unsigned>(count, kMostComponents));
  header_.width = width;
  header_.height = height;
  header_.components = static_cast<std::uint8_t>(count);
  header_.progressive = IsProgressiveFrame(marker);
  header_.grayAcross = components_[0].across;
  header_.grayDown = components_[0].down;
  if (count > 1) {
    header_.colourAcross = components_[1].across;
    header_.colourDown = components_[1].down;
  }
  headerKnown_ = true;

  bool unsupported = (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) || precision != 8 || height == 0 ||
                     (count != 1 && count != 3);
  if (count == 3) {
    const bool namedRgb = components_[0].id == 'R' && components_[1].id == 'G' && components_[2].id == 'B';
    unsupported = unsupported || adobeTransform_ == 0 || namedRgb;
    for (unsigned i = 1; i < 3; ++i) {
      unsupported =
          unsupported || components_[i].across > components_[0].across || components_[i].down > components_[0].down;
    }
  }
  if (unsupported) {
    return Fail(JpegStatus::kUnsupported);
  }
  reduction_ = ReductionTaken(header_, asked_);
  outWidth_ = WidthAt(header_, asked_);
  outHeight_ = HeightAt(header_, asked_);
  if (sink_ == nullptr) {
    return Finish();
  }
  if (RowBytesFor(header_, asked_) > rowMemory_.size()) {
    return Fail(JpegStatus::kNoRoom);
  }
  stage_ = Stage::kMarker;
  return true;
}

bool JpegReader::ReadScan(std::size_t length) noexcept {
  std::array<std::uint8_t, 1> head{};
  if (length < 1) {
    return Fail(JpegStatus::kBadScan);
  }
  if (!Fill(head)) {
    return false;
  }
  const unsigned count = head[0];
  if (count < 1 || count > kMostComponents || length != 4 + (2 * std::size_t{count})) {
    return Fail(JpegStatus::kBadScan);
  }
  bool hasGray = false;
  unsigned blocks = 0;
  for (unsigned i = 0; i < count; ++i) {
    std::array<std::uint8_t, 2> entry{};
    if (!Fill(entry)) {
      return false;
    }
    unsigned found = kMostComponents;
    for (unsigned j = 0; j < componentCount_; ++j) {
      if (components_[j].id == entry[0]) {
        found = j;
      }
    }
    if (found == kMostComponents || (entry[1] >> 4U) > 3 || (entry[1] & 15U) > 3) {
      return Fail(JpegStatus::kBadScan);
    }
    for (unsigned j = 0; j < i; ++j) {
      if (scanComponents_[j] == found) {
        return Fail(JpegStatus::kBadScan);
      }
    }
    scanComponents_[i] = static_cast<std::uint8_t>(found);
    components_[found].dcTable = static_cast<std::uint8_t>(entry[1] >> 4U);
    components_[found].acTable = static_cast<std::uint8_t>(entry[1] & 15U);
    hasGray = hasGray || found == 0;
    blocks += std::uint32_t{components_[found].across} * components_[found].down;
  }
  std::array<std::uint8_t, 3> tail{};
  if (!Fill(tail)) {
    return false;
  }
  scanCount_ = static_cast<std::uint8_t>(count);
  const unsigned start = tail[0];
  const unsigned end = tail[1];
  const unsigned high = tail[2] >> 4U;
  const unsigned low = tail[2] & 15U;
  if (count > 1 && blocks > kMostBlocksPerMcu) {
    return Fail(JpegStatus::kBadScan);
  }
  if (header_.progressive) {
    if (start > end || end > 63 || (start == 0 && end != 0) || (start > 0 && count != 1) || high > 13 || low > 13) {
      return Fail(JpegStatus::kBadScan);
    }
  }
  // The scan read is the first that has Y: in a progressive picture, the
  // first of its averages. Every other is passed over.
  const bool wanted = hasGray && (!header_.progressive || (start == 0 && high == 0));
  if (!wanted) {
    ++scansPassedOver_;
    sawFF_ = false;
    stage_ = Stage::kSkipScan;
    return true;
  }
  successive_ = static_cast<std::uint8_t>(header_.progressive ? low : 0);
  for (unsigned i = 0; i < count; ++i) {
    const Component& component = components_[scanComponents_[i]];
    if (!huffman_[component.dcTable].defined || (!header_.progressive && !huffman_[4 + component.acTable].defined)) {
      return Fail(JpegStatus::kBadScan);
    }
  }
  if (!quantizationDefined_[components_[0].table]) {
    return Fail(JpegStatus::kBadScan);
  }
  return StartRows();
}

// ---- Rows ----

bool JpegReader::StartRows() noexcept {
  blockSide_ = 8 / static_cast<std::uint32_t>(reduction_);
  interleaved_ = scanCount_ > 1;
  const Component& gray = components_[0];
  if (interleaved_) {
    // Y has the most blocks of any component, both ways.
    mcusAcross_ = (header_.width + (8U * gray.across) - 1) / (8U * gray.across);
    mcusDown_ = (header_.height + (8U * gray.down) - 1) / (8U * gray.down);
    bufferWidth_ = mcusAcross_ * gray.across * blockSide_;
    rowsPerMcu_ = gray.down * blockSide_;
  } else {
    mcusAcross_ = (header_.width + 7) / 8;
    mcusDown_ = (header_.height + 7) / 8;
    bufferWidth_ = mcusAcross_ * blockSide_;
    rowsPerMcu_ = blockSide_;
  }
  if (std::uint64_t{bufferWidth_} * rowsPerMcu_ > rowMemory_.size()) {
    return Fail(JpegStatus::kNoRoom);
  }
  for (Component& component : components_) {
    component.last = 0;
  }
  mcuX_ = 0;
  mcuY_ = 0;
  mcusToRestart_ = restartInterval_;
  nextRestart_ = 0;
  ResetBits();
  stage_ = Stage::kDecode;
  return true;
}

bool JpegReader::Decode() noexcept {
  for (std::size_t count = 0; count < kMcusPerStep; ++count) {
    if (!DecodeMcu()) {
      return false;
    }
    if (++mcuX_ == mcusAcross_) {
      mcuX_ = 0;
      ++mcuY_;
      rowsMade_ = std::min(outHeight_, rowsMade_ + rowsPerMcu_);
      stage_ = Stage::kGive;
      return true;
    }
  }
  return true;
}

bool JpegReader::Give() noexcept {
  const std::uint32_t inRow = rowsGiven_ - ((mcuY_ - 1) * rowsPerMcu_);
  sink_->Row(rowsGiven_,
             std::span<const std::uint8_t>(rowMemory_.data() + (std::size_t{inRow} * bufferWidth_), outWidth_));
  ++rowsGiven_;
  if (rowsGiven_ == outHeight_) {
    return Finish();
  }
  if (rowsGiven_ == rowsMade_) {
    stage_ = Stage::kDecode;
  }
  return true;
}

bool JpegReader::DecodeMcu() noexcept {
  if (restartInterval_ != 0) {
    if (mcusToRestart_ == 0) {
      const JpegStatus restarted = Restart();
      if (restarted != JpegStatus::kOk) {
        return Fail(restarted);
      }
      mcusToRestart_ = restartInterval_;
    }
    --mcusToRestart_;
  }
  bool decoded = true;
  if (interleaved_) {
    for (unsigned i = 0; i < scanCount_ && decoded; ++i) {
      Component& component = components_[scanComponents_[i]];
      const bool gray = scanComponents_[i] == 0;
      for (unsigned down = 0; down < component.down && decoded; ++down) {
        for (unsigned across = 0; across < component.across && decoded; ++across) {
          decoded = header_.progressive ? DecodeAverage(component, gray) : DecodeBlock(component, gray);
          if (decoded && gray) {
            MakePixels((mcuX_ * component.across) + across, down);
          }
        }
      }
    }
  } else {
    Component& gray = components_[0];
    decoded = header_.progressive ? DecodeAverage(gray, true) : DecodeBlock(gray, true);
    if (decoded) {
      MakePixels(mcuX_, 0);
    }
  }
  if (failed_) {
    return Fail(JpegStatus::kSourceFailed);
  }
  if (ranOut_) {
    // Into the end of the file or of the picture: cut short. Into any other
    // marker: data that is not as it should be.
    return Fail(dataMarker_ == 0 || dataMarker_ == kEndOfImage ? JpegStatus::kTruncated : JpegStatus::kBadData);
  }
  return decoded || Fail(JpegStatus::kBadData);
}

bool JpegReader::DecodeBlock(Component& component, bool wanted) noexcept {
  const std::int32_t size = DecodeValue(huffman_[component.dcTable]);
  if (size < 0 || size > 11) {
    return false;
  }
  component.last = std::clamp<std::int32_t>(component.last + ReceiveExtend(static_cast<unsigned>(size)), -kMostAverage,
                                            kMostAverage);
  const std::array<std::uint16_t, 64>& steps = quantization_[component.table];
  if (wanted) {
    block_.fill(0);
    block_[0] = Clamp(std::int64_t{component.last} * steps[0], kMostCoefficient);
    acSeen_ = false;
    lastAcross_ = 0;
    lastDown_ = 0;
  }
  // At an eighth the average is all that is wanted.
  const bool keep = wanted && blockSide_ > 1;
  const Huffman& ac = huffman_[4 + component.acTable];
  for (std::uint32_t k = 1; k < 64;) {
    const std::int32_t value = DecodeValue(ac);
    if (value < 0) {
      return false;
    }
    const std::uint32_t run = static_cast<std::uint32_t>(value) >> 4U;
    const std::uint32_t bits = static_cast<std::uint32_t>(value) & 15U;
    if (bits == 0) {
      if (run != 15) {
        break;  // The rest of the block is 0.
      }
      k += 16;
      continue;
    }
    k += run;
    if (k > 63 || bits > 10) {
      return false;
    }
    const std::int32_t coefficient = ReceiveExtend(bits);
    if (keep) {
      const std::uint8_t at = kNatural[k];
      block_[at] = Clamp(std::int64_t{coefficient} * steps[k], kMostCoefficient);
      acSeen_ = true;
      lastAcross_ = std::max<std::uint32_t>(lastAcross_, at & 7U);
      lastDown_ = std::max<std::uint32_t>(lastDown_, at >> 3U);
    }
    ++k;
  }
  return true;
}

bool JpegReader::DecodeAverage(Component& component, bool wanted) noexcept {
  const std::int32_t size = DecodeValue(huffman_[component.dcTable]);
  if (size < 0 || size > 11) {
    return false;
  }
  component.last = std::clamp<std::int32_t>(component.last + ReceiveExtend(static_cast<unsigned>(size)), -kMostAverage,
                                            kMostAverage);
  if (wanted) {
    const std::int64_t average = std::int64_t{component.last} * static_cast<std::int64_t>(1U << successive_);
    block_[0] = Clamp(average * quantization_[component.table][0], kMostCoefficient);
    acSeen_ = false;
  }
  return true;
}

void JpegReader::MakePixels(std::uint32_t blockX, std::uint32_t blockY) noexcept {
  const std::size_t side = blockSide_;
  std::uint8_t* out = rowMemory_.data() + (std::size_t{blockY} * side * bufferWidth_) + (std::size_t{blockX} * side);
  if (!acSeen_ || side == 1) {
    // The average alone: every pixel the same.
    const std::uint8_t pixel = ToPixel(ShiftDown(block_[0] + 4, 3));
    for (std::size_t y = 0; y < side; ++y) {
      std::fill_n(out + (y * bufferWidth_), side, pixel);
    }
    return;
  }
  if (side == 8) {
    Transform<8>(block_, work_, kWhole, lastAcross_, lastDown_, out, bufferWidth_);
  } else if (side == 4) {
    Transform<4>(block_, work_, kHalf, lastAcross_, lastDown_, out, bufferWidth_);
  } else {
    Transform<2>(block_, work_, kQuarter, lastAcross_, lastDown_, out, bufferWidth_);
  }
}

JpegStatus JpegReader::Restart() noexcept {
  // What is left of the data before the marker can only be the bits that
  // fill out its last byte.
  if (bitCount_ - zeroBits_ >= 8) {
    return JpegStatus::kBadData;
  }
  if (!dataEnded_) {
    bits_ = 0;
    bitCount_ = 0;
    zeroBits_ = 0;
    MoreBits();
  }
  if (failed_) {
    return JpegStatus::kSourceFailed;
  }
  if (!dataEnded_) {
    return JpegStatus::kBadData;
  }
  if (dataMarker_ == 0 || dataMarker_ == kEndOfImage) {
    return JpegStatus::kTruncated;
  }
  if (dataMarker_ != kFirstRestart + nextRestart_) {
    return JpegStatus::kBadData;
  }
  nextRestart_ = static_cast<std::uint8_t>((nextRestart_ + 1U) & 7U);
  for (Component& component : components_) {
    component.last = 0;
  }
  ResetBits();
  return JpegStatus::kOk;
}

// ---- Bits ----

void JpegReader::ResetBits() noexcept {
  bits_ = 0;
  bitCount_ = 0;
  zeroBits_ = 0;
  dataEnded_ = false;
  dataMarker_ = 0;
  ranOut_ = false;
}

void JpegReader::MoreBits() noexcept {
  while (bitCount_ <= 24) {
    std::uint8_t byte = 0;
    if (!dataEnded_) {
      if (!Byte(byte)) {
        dataEnded_ = true;
        dataMarker_ = 0;
      } else if (byte == 0xFF) {
        std::uint8_t next = 0xFF;
        bool more = true;
        for (std::size_t fill = 0; next == 0xFF && more && fill <= kMostFillBytes; ++fill) {
          more = Byte(next);
        }
        if (!more) {
          dataEnded_ = true;
          dataMarker_ = 0;
        } else if (next != 0x00) {
          // A marker; or fill bytes past what is allowed, taken as a marker
          // that is none of JPEG's.
          dataEnded_ = true;
          dataMarker_ = next;
        }
      }
    }
    if (dataEnded_) {
      byte = 0;
      zeroBits_ += 8;
    }
    bits_ |= std::uint32_t{byte} << (24U - bitCount_);
    bitCount_ += 8;
  }
}

std::uint32_t JpegReader::PeekBits(unsigned count) noexcept {
  if (bitCount_ < count) {
    MoreBits();
  }
  return bits_ >> (32U - count);
}

void JpegReader::SkipBits(unsigned count) noexcept {
  const unsigned real = bitCount_ - zeroBits_;
  if (count > real) {
    ranOut_ = true;
    zeroBits_ -= count - real;
  }
  bits_ <<= count;
  bitCount_ -= count;
}

std::int32_t JpegReader::ReceiveExtend(unsigned count) noexcept {
  if (count == 0) {
    return 0;
  }
  const std::uint32_t bits = PeekBits(count);
  SkipBits(count);
  // T.81 F.2.2.1: below half the range is the negative half.
  const auto value = static_cast<std::int32_t>(bits);
  if (bits < (1U << (count - 1))) {
    return value - static_cast<std::int32_t>((1U << count) - 1U);
  }
  return value;
}

std::int32_t JpegReader::DecodeValue(const Huffman& table) noexcept {
  const std::uint32_t first = PeekBits(8);
  if (table.fastLength[first] != 0) {
    SkipBits(table.fastLength[first]);
    return table.fastValue[first];
  }
  const std::uint32_t next = PeekBits(16);
  for (unsigned length = 9; length <= 16; ++length) {
    const auto code = static_cast<std::int32_t>(next >> (16U - length));
    if (code <= table.largest[length]) {
      SkipBits(length);
      const std::int32_t index = table.offset[length] + code;
      return table.values[static_cast<std::size_t>(index)];
    }
  }
  return -1;
}

}  // namespace bn::draw
