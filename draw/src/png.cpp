#include "draw/png.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>

#include "core/checksum.h"
#include "draw/gray.h"

namespace bn::draw {

namespace {

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
// The longest a chunk may be, as PNG has it.
constexpr std::uint32_t kLongestChunk = 0x7FFFFFFF;
// The longest palette, and the most alphas `tRNS` can give it.
constexpr std::uint32_t kMostPaletteBytes = 768;
constexpr std::size_t kMostEntries = 256;

using Name = std::array<std::uint8_t, 4>;
constexpr Name kHeaderName = {'I', 'H', 'D', 'R'};
constexpr Name kPaletteName = {'P', 'L', 'T', 'E'};
constexpr Name kDataName = {'I', 'D', 'A', 'T'};
constexpr Name kEndName = {'I', 'E', 'N', 'D'};
constexpr Name kTransparencyName = {'t', 'R', 'N', 'S'};

constexpr std::uint32_t LoadBe32(std::span<const std::uint8_t, 4> in) noexcept {
  return (static_cast<std::uint32_t>(in[0]) << 24U) | (static_cast<std::uint32_t>(in[1]) << 16U) |
         (static_cast<std::uint32_t>(in[2]) << 8U) | static_cast<std::uint32_t>(in[3]);
}

constexpr std::uint16_t LoadBe16(std::span<const std::uint8_t, 2> in) noexcept {
  return static_cast<std::uint16_t>((static_cast<unsigned>(in[0]) << 8U) | static_cast<unsigned>(in[1]));
}

bool Is(std::span<const std::uint8_t, 4> name, const Name& wanted) noexcept {
  return std::equal(name.begin(), name.end(), wanted.begin());
}

// A chunk's name is four letters, each a capital or a small one.
bool IsName(std::span<const std::uint8_t, 4> name) noexcept {
  return std::all_of(name.begin(), name.end(), [](std::uint8_t letter) {
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
  });
}

// A chunk whose name begins with a capital is one a reader has to
// understand to read the picture.
bool IsCritical(std::span<const std::uint8_t, 4> name) noexcept { return name[0] >= 'A' && name[0] <= 'Z'; }

// Whether a kind of picture may have samples of `depth` bits.
bool DepthAllowed(PngColour colour, std::uint8_t depth) noexcept {
  switch (colour) {
    case PngColour::kGray:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PngColour::kPalette:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case PngColour::kRgb:
    case PngColour::kGrayAlpha:
    case PngColour::kRgba:
      return depth == 8 || depth == 16;
  }
  return false;
}

bool HeaderAllowed(const PngHeader& header) noexcept {
  return header.width >= 1 && header.width <= PngReader::kMaxSide && header.height >= 1 &&
         header.height <= PngReader::kMaxSide && DepthAllowed(header.colour, header.depth);
}

// A sample of `depth` bits, fewer than 8, from a row: the one at `index`,
// counting from the most significant bits of the row's first byte.
std::uint8_t PackedSample(std::span<const std::uint8_t> row, std::size_t index, unsigned depth) noexcept {
  const std::size_t bit = index * depth;
  const std::size_t shift = std::size_t{8} - depth - (bit & 7U);
  return static_cast<std::uint8_t>((static_cast<unsigned>(row[bit >> 3U]) >> shift) & ((1U << depth) - 1U));
}

// The predictor of PNG's fourth filter: whichever of the bytes to the
// left, above, and above to the left is nearest to left + above - corner.
std::uint8_t Paeth(std::uint8_t left, std::uint8_t above, std::uint8_t corner) noexcept {
  const int guess = int{left} + int{above} - int{corner};
  const int toLeft = std::abs(guess - int{left});
  const int toAbove = std::abs(guess - int{above});
  const int toCorner = std::abs(guess - int{corner});
  if (toLeft <= toAbove && toLeft <= toCorner) {
    return left;
  }
  return toAbove <= toCorner ? above : corner;
}

}  // namespace

const char* PngStatusName(PngStatus status) noexcept {
  switch (status) {
    case PngStatus::kOk:
      return "ok";
    case PngStatus::kNotPng:
      return "not png";
    case PngStatus::kBadHeader:
      return "bad header";
    case PngStatus::kBadChecksum:
      return "bad checksum";
    case PngStatus::kBadChunk:
      return "bad chunk";
    case PngStatus::kUnknownChunk:
      return "unknown chunk";
    case PngStatus::kNoPalette:
      return "no palette";
    case PngStatus::kBadData:
      return "bad data";
    case PngStatus::kTruncated:
      return "truncated";
    case PngStatus::kInterlaced:
      return "interlaced";
    case PngStatus::kNoRoom:
      return "no room";
    case PngStatus::kSourceFailed:
      return "source failed";
  }
  return "unknown";
}

const char* PngColourName(PngColour colour) noexcept {
  switch (colour) {
    case PngColour::kGray:
      return "gray";
    case PngColour::kRgb:
      return "rgb";
    case PngColour::kPalette:
      return "palette";
    case PngColour::kGrayAlpha:
      return "gray+alpha";
    case PngColour::kRgba:
      return "rgba";
  }
  return "unknown";
}

std::uint32_t PngHeader::Samples() const noexcept {
  switch (colour) {
    case PngColour::kGray:
    case PngColour::kPalette:
      return 1;
    case PngColour::kGrayAlpha:
      return 2;
    case PngColour::kRgb:
      return 3;
    case PngColour::kRgba:
      return 4;
  }
  return 1;
}

std::uint64_t PngHeader::Stride() const noexcept { return ((std::uint64_t{width} * Samples() * depth) + 7U) / 8U; }

std::uint64_t PngReader::RowBytesFor(const PngHeader& header) noexcept {
  if (!HeaderAllowed(header)) {
    return 0;
  }
  return 2U * std::max<std::uint64_t>(header.Stride() + 1U, header.width);
}

PngReader::PngReader(std::span<std::uint8_t> window, std::span<std::uint8_t> rows) noexcept
    : window_(window), rowMemory_(rows), inflater_(data_, window) {}

void PngReader::Begin(core::ByteSource& source, RowSink* sink) noexcept {
  source_ = &source;
  sink_ = sink;
  status_ = PngStatus::kOk;
  stage_ = Stage::kSignature;
  header_ = PngHeader{};
  headerKnown_ = false;
  seenPalette_ = false;
  seenTransparency_ = false;
  paletteEntries_ = 0;
  hasKey_ = false;
  key_ = {};
  paletteGray_.fill(0);
  chunk_ = Chunk::kOther;
  chunkLeft_ = 0;
  chunkLength_ = 0;
  crc_ = 0;
  current_ = {};
  previous_ = {};
  rowLength_ = 0;
  stride_ = 0;
  filled_ = 0;
  rowsGiven_ = 0;
  passedOver_ = 0;
  badPassedOver_ = 0;
}

bool PngReader::Step() noexcept {
  switch (stage_) {
    case Stage::kSignature:
      return ReadSignature();
    case Stage::kChunkStart:
      return StartChunk();
    case Stage::kChunkData:
      return ReadChunkData();
    case Stage::kDataStart:
      return StartData();
    case Stage::kRows:
      return ReadRow();
    case Stage::kDone:
      return false;
  }
  return false;
}

PngStatus PngReader::Read(core::ByteSource& source, RowSink* sink) noexcept {
  Begin(source, sink);
  while (Step()) {
  }
  return status_;
}

bool PngReader::Transparent() const noexcept {
  return header_.colour == PngColour::kGrayAlpha || header_.colour == PngColour::kRgba || seenTransparency_;
}

bool PngReader::Fill(std::span<std::uint8_t> dst) noexcept {
  std::size_t got = 0;
  while (got < dst.size()) {
    const std::ptrdiff_t read = source_->Read(dst.subspan(got));
    if (read < 0 || static_cast<std::size_t>(read) > dst.size() - got) {
      return Fail(PngStatus::kSourceFailed);
    }
    if (read == 0) {
      return Fail(PngStatus::kTruncated);
    }
    got += static_cast<std::size_t>(read);
  }
  return true;
}

bool PngReader::ReadSignature() noexcept {
  std::array<std::uint8_t, kSignatureBytes> bytes{};
  if (!Fill(bytes)) {
    // A file too short to have a signature is not a picture.
    return status_ == PngStatus::kTruncated ? Fail(PngStatus::kNotPng) : false;
  }
  if (bytes != kSignature) {
    return Fail(PngStatus::kNotPng);
  }
  stage_ = Stage::kChunkStart;
  return true;
}

bool PngReader::StartChunk() noexcept {
  std::array<std::uint8_t, kChunkStartBytes> bytes{};
  if (!Fill(bytes)) {
    return false;
  }
  const std::uint32_t length = LoadBe32(std::span(bytes).first<4>());
  const std::span<const std::uint8_t, 4> name = std::span(bytes).last<4>();
  if (length > kLongestChunk || !IsName(name)) {
    return Fail(headerKnown_ ? PngStatus::kBadChunk : PngStatus::kBadHeader);
  }
  if (!headerKnown_) {
    if (!Is(name, kHeaderName) || length != kHeaderBytes) {
      return Fail(PngStatus::kBadHeader);
    }
    chunk_ = Chunk::kHeader;
  } else if (Is(name, kHeaderName)) {
    return Fail(PngStatus::kBadChunk);
  } else if (Is(name, kDataName)) {
    if (header_.colour == PngColour::kPalette && !seenPalette_) {
      return Fail(PngStatus::kNoPalette);
    }
    data_.Start(*source_, length);
    stage_ = Stage::kDataStart;
    return true;
  } else if (Is(name, kEndName)) {
    // The end, before any of the picture's data.
    return Fail(PngStatus::kTruncated);
  } else if (Is(name, kPaletteName)) {
    // Only a picture of palette colours uses its palette. A colour picture
    // may suggest one for showing it with fewer colours, and that is
    // passed over; so is one in a gray picture, which should not have one.
    if (header_.colour != PngColour::kPalette) {
      chunk_ = Chunk::kOther;
    } else if (seenPalette_ || length == 0 || length % 3 != 0 || length > kMostPaletteBytes) {
      return Fail(PngStatus::kBadChunk);
    } else {
      chunk_ = Chunk::kPalette;
    }
  } else if (Is(name, kTransparencyName)) {
    // Taken only if it is the first, it says what this kind of picture can
    // use, and, for a palette, comes after the palette and gives it no more
    // alphas than it has entries. Otherwise passed over.
    bool usable = false;
    if (!seenTransparency_) {
      switch (header_.colour) {
        case PngColour::kGray:
          usable = length == 2;
          break;
        case PngColour::kRgb:
          usable = length == 6;
          break;
        case PngColour::kPalette:
          usable = seenPalette_ && length <= paletteEntries_;
          break;
        case PngColour::kGrayAlpha:
        case PngColour::kRgba:
          break;
      }
    }
    chunk_ = usable ? Chunk::kTransparency : Chunk::kOther;
  } else if (IsCritical(name)) {
    return Fail(PngStatus::kUnknownChunk);
  } else {
    chunk_ = Chunk::kOther;
  }
  chunkLength_ = length;
  chunkLeft_ = length;
  crc_ = core::Crc32(name);
  stage_ = Stage::kChunkData;
  return true;
}

bool PngReader::ReadChunkData() noexcept {
  // A chunk that is kept is read whole in one call: the header is 13
  // bytes, a palette no more than 768 and its alphas no more than 256. One
  // that is passed over is read through the window, which is not in use
  // until the picture's data begins, kChunkBytes at a time. That is only
  // done when rows are wanted, so the window is there.
  const std::size_t take = std::min<std::size_t>(chunkLeft_, kChunkBytes);
  const std::span<std::uint8_t> bytes =
      chunk_ == Chunk::kHeader ? std::span<std::uint8_t>(headerBytes_) : window_.first(take);
  if (!Fill(bytes.first(take))) {
    return false;
  }
  crc_ = core::Crc32(bytes.first(take), crc_);
  chunkLeft_ -= static_cast<std::uint32_t>(take);
  if (chunkLeft_ > 0) {
    return true;
  }
  std::array<std::uint8_t, kChecksumBytes> stored{};
  if (!Fill(stored)) {
    return false;
  }
  return TakeChunk(LoadBe32(stored) == crc_, bytes.first(take));
}

bool PngReader::TakeChunk(bool sound, std::span<const std::uint8_t> bytes) noexcept {
  switch (chunk_) {
    case Chunk::kHeader:
      return sound ? TakeHeader() : Fail(PngStatus::kBadChecksum);
    case Chunk::kPalette:
      if (!sound) {
        return Fail(PngStatus::kBadChecksum);
      }
      TakePalette(bytes);
      break;
    case Chunk::kTransparency:
      if (sound) {
        TakeTransparency(bytes);
      } else {
        ++passedOver_;
        ++badPassedOver_;
      }
      break;
    case Chunk::kOther:
      ++passedOver_;
      badPassedOver_ += sound ? 0U : 1U;
      break;
  }
  stage_ = Stage::kChunkStart;
  return true;
}

bool PngReader::TakeHeader() noexcept {
  const std::span<const std::uint8_t, kHeaderBytes> bytes(headerBytes_);
  PngHeader header;
  header.width = LoadBe32(bytes.subspan<0, 4>());
  header.height = LoadBe32(bytes.subspan<4, 4>());
  header.depth = bytes[8];
  const std::uint8_t colour = bytes[9];
  const std::uint8_t compression = bytes[10];
  const std::uint8_t filtering = bytes[11];
  const std::uint8_t interlace = bytes[12];
  if (colour != 0 && colour != 2 && colour != 3 && colour != 4 && colour != 6) {
    return Fail(PngStatus::kBadHeader);
  }
  header.colour = static_cast<PngColour>(colour);
  header.interlaced = interlace == 1;
  if (compression != 0 || filtering != 0 || interlace > 1 || !HeaderAllowed(header)) {
    return Fail(PngStatus::kBadHeader);
  }
  header_ = header;
  headerKnown_ = true;
  if (sink_ == nullptr) {
    return Finish();
  }
  if (header_.interlaced) {
    return Fail(PngStatus::kInterlaced);
  }
  const std::uint64_t needed = RowBytesFor(header_);
  if (window_.size() < kWindowBytes || needed > rowMemory_.size()) {
    return Fail(PngStatus::kNoRoom);
  }
  rowLength_ = static_cast<std::size_t>(needed / 2U);
  stride_ = static_cast<std::size_t>(header_.Stride());
  current_ = rowMemory_.first(rowLength_);
  previous_ = rowMemory_.subspan(rowLength_, rowLength_);
  stage_ = Stage::kChunkStart;
  return true;
}

void PngReader::TakePalette(std::span<const std::uint8_t> bytes) noexcept {
  paletteEntries_ = static_cast<std::uint16_t>(bytes.size() / 3U);
  for (std::size_t entry = 0; entry < paletteEntries_; ++entry) {
    paletteGray_[entry] = GrayOf(bytes[entry * 3U], bytes[(entry * 3U) + 1U], bytes[(entry * 3U) + 2U]);
  }
  seenPalette_ = true;
}

void PngReader::TakeTransparency(std::span<const std::uint8_t> bytes) noexcept {
  seenTransparency_ = true;
  if (header_.colour == PngColour::kPalette) {
    for (std::size_t entry = 0; entry < bytes.size() && entry < kMostEntries; ++entry) {
      paletteGray_[entry] = OverWhite(paletteGray_[entry], bytes[entry]);
    }
    return;
  }
  hasKey_ = true;
  for (std::size_t sample = 0; sample * 2U < bytes.size() && sample < key_.size(); ++sample) {
    key_[sample] = LoadBe16(bytes.subspan(sample * 2U).first<2>());
  }
}

bool PngReader::StartData() noexcept {
  // zlib's two bytes in front of the compressed data: how it is
  // compressed (DEFLATE, with a window of no more than 32 KB), and a check
  // on those, with no dictionary to begin from.
  std::array<std::uint8_t, 2> start{};
  std::size_t got = 0;
  while (got < start.size()) {
    const std::ptrdiff_t read = data_.Read(std::span(start).subspan(got));
    if (read <= 0) {
      return Fail(data_.Status() != PngStatus::kOk ? data_.Status() : PngStatus::kTruncated);
    }
    got += static_cast<std::size_t>(read);
  }
  const unsigned method = start[0] & 0x0FU;
  const unsigned windowBits = start[0] >> 4U;
  const unsigned check = (static_cast<unsigned>(start[0]) << 8U) | start[1];
  const bool dictionary = (start[1] & 0x20U) != 0;
  if (method != 8 || windowBits > 7 || check % 31U != 0 || dictionary) {
    return Fail(PngStatus::kBadData);
  }
  inflater_.Reset(data_);
  // The row above the first is nothing but zeros, for the filters.
  std::fill(previous_.begin(), previous_.end(), std::uint8_t{0});
  filled_ = 0;
  stage_ = Stage::kRows;
  return true;
}

bool PngReader::ReadRow() noexcept {
  const std::size_t want = std::min(stride_ + 1U - filled_, kRowBytesPerStep);
  const std::ptrdiff_t read = inflater_.Read(current_.subspan(filled_, want));
  if (read < 0) {
    return Fail(DataFailure());
  }
  if (read == 0) {
    // The data ended, as data should, before the picture did.
    return Fail(PngStatus::kTruncated);
  }
  filled_ += static_cast<std::size_t>(read);
  if (filled_ < stride_ + 1U) {
    return true;
  }
  return FinishRow();
}

bool PngReader::FinishRow() noexcept {
  const std::uint8_t filter = current_[0];
  if (filter > 4) {
    return Fail(PngStatus::kBadData);
  }
  Unfilter(filter);
  // The row above is not needed again: its memory takes this row's gray,
  // and this row becomes the one above the next.
  ToGray(current_.subspan(1, stride_), previous_.first(header_.width));
  std::swap(current_, previous_);
  sink_->Row(rowsGiven_, current_.first(header_.width));
  ++rowsGiven_;
  filled_ = 0;
  if (rowsGiven_ == header_.height) {
    return Finish();
  }
  return true;
}

void PngReader::Unfilter(std::uint8_t filter) noexcept {
  const std::span<std::uint8_t> row = current_.subspan(1, stride_);
  const std::span<const std::uint8_t> above = previous_.subspan(1, stride_);
  // How far back the byte "to the left" is: a whole pixel's bytes, or one
  // byte for a pixel smaller than that.
  const std::size_t back = std::max<std::size_t>(1, (header_.Samples() * header_.depth) / 8U);
  switch (filter) {
    case 1:  // Sub: the difference from the byte to the left.
      for (std::size_t at = back; at < row.size(); ++at) {
        row[at] = static_cast<std::uint8_t>(row[at] + row[at - back]);
      }
      break;
    case 2:  // Up: from the byte above.
      for (std::size_t at = 0; at < row.size(); ++at) {
        row[at] = static_cast<std::uint8_t>(row[at] + above[at]);
      }
      break;
    case 3:  // Average: from the mean of the bytes to the left and above.
      for (std::size_t at = 0; at < row.size(); ++at) {
        const unsigned left = at >= back ? row[at - back] : 0U;
        row[at] = static_cast<std::uint8_t>(row[at] + ((left + above[at]) / 2U));
      }
      break;
    case 4:  // Paeth: from whichever of left, above and the corner is nearest a guess.
      for (std::size_t at = 0; at < row.size(); ++at) {
        const std::uint8_t left = at >= back ? row[at - back] : std::uint8_t{0};
        const std::uint8_t corner = at >= back ? above[at - back] : std::uint8_t{0};
        row[at] = static_cast<std::uint8_t>(row[at] + Paeth(left, above[at], corner));
      }
      break;
    default:  // None: the bytes as they are.
      break;
  }
}

void PngReader::ToGray(std::span<const std::uint8_t> row, std::span<std::uint8_t> gray) const noexcept {
  const unsigned depth = header_.depth;
  switch (header_.colour) {
    case PngColour::kGray:
      if (depth == 16) {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          const std::uint16_t sample = LoadBe16(row.subspan(x * 2U).first<2>());
          gray[x] = hasKey_ && sample == key_[0] ? std::uint8_t{255} : row[x * 2U];
        }
      } else if (depth == 8) {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          gray[x] = hasKey_ && row[x] == key_[0] ? std::uint8_t{255} : row[x];
        }
      } else {
        // 1, 2 or 4 bits: stretched so that the largest is white (by 255,
        // 85 or 17).
        const unsigned stretch = 255U / ((1U << depth) - 1U);
        for (std::size_t x = 0; x < gray.size(); ++x) {
          const std::uint8_t sample = PackedSample(row, x, depth);
          gray[x] = hasKey_ && sample == key_[0] ? std::uint8_t{255} : static_cast<std::uint8_t>(sample * stretch);
        }
      }
      break;
    case PngColour::kRgb:
      if (depth == 16) {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          const std::span<const std::uint8_t> pixel = row.subspan(x * 6U, 6);
          const bool none = hasKey_ && LoadBe16(pixel.first<2>()) == key_[0] &&
                            LoadBe16(pixel.subspan<2, 2>()) == key_[1] && LoadBe16(pixel.subspan<4, 2>()) == key_[2];
          gray[x] = none ? std::uint8_t{255} : GrayOf(pixel[0], pixel[2], pixel[4]);
        }
      } else {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          const std::span<const std::uint8_t> pixel = row.subspan(x * 3U, 3);
          const bool none = hasKey_ && pixel[0] == key_[0] && pixel[1] == key_[1] && pixel[2] == key_[2];
          gray[x] = none ? std::uint8_t{255} : GrayOf(pixel[0], pixel[1], pixel[2]);
        }
      }
      break;
    case PngColour::kPalette:
      if (depth == 8) {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          gray[x] = paletteGray_[row[x]];
        }
      } else {
        for (std::size_t x = 0; x < gray.size(); ++x) {
          gray[x] = paletteGray_[PackedSample(row, x, depth)];
        }
      }
      break;
    case PngColour::kGrayAlpha: {
      const std::size_t bytes = depth / 4U;  // A pixel's: 2 or 4.
      for (std::size_t x = 0; x < gray.size(); ++x) {
        const std::span<const std::uint8_t> pixel = row.subspan(x * bytes, bytes);
        gray[x] = OverWhite(pixel[0], pixel[bytes / 2U]);
      }
      break;
    }
    case PngColour::kRgba: {
      const std::size_t sample = depth / 8U;  // A sample's bytes: 1 or 2.
      for (std::size_t x = 0; x < gray.size(); ++x) {
        const std::span<const std::uint8_t> pixel = row.subspan(x * sample * 4U, sample * 4U);
        gray[x] = OverWhite(GrayOf(pixel[0], pixel[sample], pixel[sample * 2U]), pixel[sample * 3U]);
      }
      break;
    }
  }
}

PngStatus PngReader::DataFailure() const noexcept {
  if (data_.Status() != PngStatus::kOk) {
    return data_.Status();
  }
  switch (inflater_.CurrentStatus()) {
    case core::Inflater::Status::kTruncated:
      return PngStatus::kTruncated;
    case core::Inflater::Status::kNoWindow:
      return PngStatus::kNoRoom;
    case core::Inflater::Status::kSourceFailed:
      return PngStatus::kSourceFailed;
    case core::Inflater::Status::kBadData:
    case core::Inflater::Status::kRunning:
    case core::Inflater::Status::kDone:
      break;
  }
  return PngStatus::kBadData;
}

bool PngReader::Fail(PngStatus status) noexcept {
  status_ = status;
  stage_ = Stage::kDone;
  return false;
}

bool PngReader::Finish() noexcept {
  stage_ = Stage::kDone;
  return false;
}

// ---- The compressed data, chunk after chunk ----

void PngReader::DataSource::Start(core::ByteSource& source, std::uint32_t length) noexcept {
  source_ = &source;
  status_ = PngStatus::kOk;
  ended_ = false;
  left_ = length;
  crc_ = core::Crc32(kDataName);
  emptyInARow_ = length == 0 ? 1U : 0U;
}

std::ptrdiff_t PngReader::DataSource::Read(std::span<std::uint8_t> dst) noexcept {
  if (status_ != PngStatus::kOk) {
    return kError;
  }
  while (!ended_ && left_ == 0) {
    if (!NextChunk()) {
      return status_ == PngStatus::kOk ? 0 : kError;
    }
  }
  if (ended_ || dst.empty()) {
    return 0;
  }
  const std::size_t want = std::min<std::size_t>(dst.size(), left_);
  const std::ptrdiff_t read = source_->Read(dst.first(want));
  if (read < 0 || static_cast<std::size_t>(read) > want) {
    Fail(PngStatus::kSourceFailed);
    return kError;
  }
  if (read == 0) {
    // The file ends inside the chunk: so does the data.
    ended_ = true;
    return 0;
  }
  crc_ = core::Crc32(dst.first(static_cast<std::size_t>(read)), crc_);
  left_ -= static_cast<std::uint32_t>(read);
  return read;
}

bool PngReader::DataSource::NextChunk() noexcept {
  // The checksum of the chunk that has ended, and the start of the next.
  std::array<std::uint8_t, kChunkSeam> seam{};
  std::size_t got = 0;
  while (got < seam.size()) {
    const std::ptrdiff_t read = source_->Read(std::span(seam).subspan(got));
    if (read < 0 || static_cast<std::size_t>(read) > seam.size() - got) {
      return Fail(PngStatus::kSourceFailed);
    }
    if (read == 0) {
      break;
    }
    got += static_cast<std::size_t>(read);
  }
  if (got >= kChecksumBytes && LoadBe32(std::span(seam).first<4>()) != crc_) {
    return Fail(PngStatus::kBadChecksum);
  }
  const std::span<const std::uint8_t, 4> name = std::span(seam).last<4>();
  if (got < seam.size() || !Is(name, kDataName)) {
    // The file ends, or a chunk that is not data begins: the data ends.
    ended_ = true;
    return false;
  }
  const std::uint32_t length = LoadBe32(std::span(seam).subspan<4, 4>());
  if (length > kLongestChunk) {
    return Fail(PngStatus::kBadChunk);
  }
  left_ = length;
  crc_ = core::Crc32(name);
  if (length > 0) {
    emptyInARow_ = 0;
  } else if (++emptyInARow_ > kMaxEmptyChunksInARow) {
    return Fail(PngStatus::kBadData);
  }
  return true;
}

bool PngReader::DataSource::Fail(PngStatus status) noexcept {
  status_ = status;
  ended_ = true;
  return false;
}

}  // namespace bn::draw
