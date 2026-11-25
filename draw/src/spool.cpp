#include "draw/spool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::draw {

namespace {

// What a record is, in its first byte. A chunk ends at its end or at a
// byte of nothing.
enum Tag : std::uint8_t {
  kEnd = 0,
  kLine = 1,        // From (x, y) to (x, y), four numbers of four bytes.
  kLineOn = 2,      // From where the last ended, to there and (dx, dy), two of two bytes.
  kLineOnFar = 3,   // From where the last ended, to (x, y), two of four bytes.
  kFill = 4,        // The rule, the gray and the opacity.
  kFillBox = 5,     // And the box, four numbers of four bytes.
  kFillRamp = 6,    // The rule, the opacity, a byte of nothing and the ramp.
  kFillRampBox = 7  // And the box.
};

// A ramp: three numbers of eight bytes, how many stops, how it spreads,
// and for each stop its place in four bytes, its gray and how solid.
constexpr std::size_t kRampBytes = 24 + 2 + (GrayRamp::kMostStops * 6);
constexpr std::size_t kMostRecord = 4 + kRampBytes + 16;
static_assert(kMostRecord < SpoolWriter::kChunkBytes);

// A record, built a number at a time, the low byte first.
class Built {
 public:
  explicit Built(Tag tag) noexcept { Byte(tag); }
  void Byte(std::uint8_t value) noexcept { bytes_[size_++] = value; }
  void Number(std::uint64_t value, std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
      Byte(static_cast<std::uint8_t>(value >> (8 * i)));
    }
  }
  void Four(std::int32_t value) noexcept { Number(static_cast<std::uint32_t>(value), 4); }
  void Box(Point from, Point to) noexcept {
    Four(from.x);
    Four(from.y);
    Four(to.x);
    Four(to.y);
  }
  void Ramp(const GrayRamp& ramp) noexcept {
    Number(static_cast<std::uint64_t>(ramp.at), 8);
    Number(static_cast<std::uint64_t>(ramp.across), 8);
    Number(static_cast<std::uint64_t>(ramp.down), 8);
    Byte(static_cast<std::uint8_t>(std::min(ramp.stops, GrayRamp::kMostStops)));
    Byte(static_cast<std::uint8_t>(ramp.spread));
    for (std::size_t i = 0; i < GrayRamp::kMostStops; ++i) {
      Four(ramp.offsets[i]);
      Byte(ramp.grays[i]);
      Byte(ramp.alphas[i]);
    }
  }
  std::span<const std::uint8_t> Bytes() const noexcept { return {bytes_.data(), size_}; }

 private:
  std::array<std::uint8_t, kMostRecord> bytes_{};
  std::size_t size_ = 0;
};

// A record being read back. Reading past its chunk's end says so.
class Reading {
 public:
  Reading(std::span<const std::uint8_t> bytes, std::size_t at) noexcept : bytes_(bytes), at_(at) {}
  bool Fits(std::size_t count) const noexcept { return at_ + count <= bytes_.size(); }
  std::uint8_t Byte() noexcept { return bytes_[at_++]; }
  std::uint64_t Number(std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      value |= std::uint64_t{Byte()} << (8 * i);
    }
    return value;
  }
  std::int32_t Four() noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(Number(4))); }
  std::int16_t Two() noexcept { return static_cast<std::int16_t>(static_cast<std::uint16_t>(Number(2))); }
  Point Place() noexcept {
    const std::int32_t x = Four();
    return {.x = x, .y = Four()};
  }
  bool Ramp(GrayRamp& ramp) noexcept {
    ramp.at = static_cast<std::int64_t>(Number(8));
    ramp.across = static_cast<std::int64_t>(Number(8));
    ramp.down = static_cast<std::int64_t>(Number(8));
    ramp.stops = Byte();
    const std::uint8_t spread = Byte();
    if (ramp.stops > GrayRamp::kMostStops || spread > static_cast<std::uint8_t>(GrayRamp::Spread::kRepeat)) {
      return false;
    }
    ramp.spread = static_cast<GrayRamp::Spread>(spread);
    for (std::size_t i = 0; i < GrayRamp::kMostStops; ++i) {
      ramp.offsets[i] = Four();
      ramp.grays[i] = Byte();
      ramp.alphas[i] = Byte();
    }
    ramp.Begin();
    return true;
  }
  std::size_t At() const noexcept { return at_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t at_ = 0;
};

// How long each record is, by its tag; nothing for one there is not.
constexpr std::size_t RecordBytes(std::uint8_t tag) noexcept {
  switch (tag) {
    case kLine:
      return 17;
    case kLineOn:
      return 5;
    case kLineOnFar:
      return 9;
    case kFill:
      return 4;
    case kFillBox:
      return 20;
    case kFillRamp:
      return 4 + kRampBytes;
    case kFillRampBox:
      return 4 + kRampBytes + 16;
    default:
      return 0;
  }
}

}  // namespace

const char* SpoolStatusName(SpoolStatus status) noexcept {
  switch (status) {
    case SpoolStatus::kOk:
      return "ok";
    case SpoolStatus::kNoRoom:
      return "no room";
    case SpoolStatus::kStoreFailed:
      return "store failed";
    case SpoolStatus::kBadRecord:
      return "bad record";
  }
  return "?";
}

bool SpoolWriter::Begin(std::int32_t width, std::int32_t height, std::int32_t bandRows, std::span<std::uint8_t> buffers,
                        std::span<std::uint16_t> links, SpoolStore& store) noexcept {
  const std::size_t bands = BandsFor(height, bandRows);
  recording_ = false;
  finished_ = false;
  if (width <= 0 || bands == 0 || bands > kMostBands || buffers.size() < BufferBytesFor(bands) || links.empty() ||
      static_cast<std::size_t>(bandRows) > Filler::kMostRows) {
    return false;
  }
  store_ = &store;
  buffers_ = buffers.first(BufferBytesFor(bands));
  links_ = links.first(std::min(links.size(), kMostChunks));
  width_ = width;
  height_ = height;
  bandHeight_ = bandRows * 256;
  bands_ = bands;
  band_ = {};
  chunks_ = 0;
  lines_ = 0;
  fills_ = 0;
  status_ = SpoolStatus::kOk;
  start_ = {};
  current_ = {};
  recording_ = true;
  return true;
}

SpoolStatus SpoolWriter::Finish() noexcept {
  if (recording_) {
    for (std::size_t band = 0; band < bands_; ++band) {
      Flush(band);
    }
    recording_ = false;
    finished_ = true;
  }
  return status_;
}

void SpoolWriter::Fail(SpoolStatus status) noexcept {
  if (status_ == SpoolStatus::kOk) {
    status_ = status;
  }
  recording_ = false;
}

void SpoolWriter::MoveTo(Point to) noexcept {
  CloseOutline();
  start_ = Filler::PlaceOf(to);
  current_ = start_;
}

void SpoolWriter::LineTo(Point to) noexcept {
  const Place place = Filler::PlaceOf(to);
  Line(current_, place);
  current_ = place;
}

void SpoolWriter::Close() noexcept { CloseOutline(); }

void SpoolWriter::CloseOutline() noexcept {
  if (current_ != start_) {
    Line(current_, start_);
    current_ = start_;
  }
}

void SpoolWriter::Line(Place from, Place to) noexcept {
  // A Filler adds nothing for a line along a row, nor for one that lies
  // wholly above or below its strip.
  if (!recording_ || from.y == to.y) {
    return;
  }
  // Which bands it crosses. Worked out in 32 bits, which hold a place and
  // a band's height: the device divides those far more quickly than 64.
  const std::int32_t low = std::min(from.y, to.y);
  const std::int32_t high = std::max(from.y, to.y);
  if (high <= 0) {
    return;
  }
  const std::size_t first = low <= 0 ? 0 : static_cast<std::size_t>(low / bandHeight_);
  const std::size_t last = std::min(static_cast<std::size_t>((high - 1) / bandHeight_), bands_ - 1);
  for (std::size_t band = first; band <= last && recording_; ++band) {
    Band& kept = band_[band];
    const std::int64_t dx = std::int64_t{to.x} - from.x;
    const std::int64_t dy = std::int64_t{to.y} - from.y;
    if (kept.hasEnd && kept.end == from && dx >= INT16_MIN && dx <= INT16_MAX && dy >= INT16_MIN && dy <= INT16_MAX) {
      Built record(kLineOn);
      record.Number(static_cast<std::uint16_t>(static_cast<std::int16_t>(dx)), 2);
      record.Number(static_cast<std::uint16_t>(static_cast<std::int16_t>(dy)), 2);
      Record(band, record.Bytes());
    } else if (kept.hasEnd && kept.end == from) {
      Built record(kLineOnFar);
      record.Four(to.x);
      record.Four(to.y);
      Record(band, record.Bytes());
    } else {
      Built record(kLine);
      record.Four(from.x);
      record.Four(from.y);
      record.Four(to.x);
      record.Four(to.y);
      Record(band, record.Bytes());
    }
    kept.hasEnd = true;
    kept.end = to;
    kept.pending = true;
    ++lines_;
  }
}

void SpoolWriter::Filling(std::span<const std::uint8_t> record) noexcept {
  CloseOutline();
  for (std::size_t band = 0; band < bands_ && recording_; ++band) {
    if (band_[band].pending) {
      Record(band, record);
      band_[band].pending = false;
      ++fills_;
    }
  }
}

void SpoolWriter::Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity) noexcept {
  Built record(kFill);
  record.Byte(static_cast<std::uint8_t>(rule));
  record.Byte(gray);
  record.Byte(opacity);
  Filling(record.Bytes());
}

void SpoolWriter::Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity, Point from, Point to) noexcept {
  Built record(kFillBox);
  record.Byte(static_cast<std::uint8_t>(rule));
  record.Byte(gray);
  record.Byte(opacity);
  record.Box(from, to);
  Filling(record.Bytes());
}

void SpoolWriter::Fill(FillRule rule, const GrayRamp& ramp, std::uint8_t opacity) noexcept {
  Built record(kFillRamp);
  record.Byte(static_cast<std::uint8_t>(rule));
  record.Byte(opacity);
  record.Byte(0);
  record.Ramp(ramp);
  Filling(record.Bytes());
}

void SpoolWriter::Fill(FillRule rule, const GrayRamp& ramp, std::uint8_t opacity, Point from, Point to) noexcept {
  Built record(kFillRampBox);
  record.Byte(static_cast<std::uint8_t>(rule));
  record.Byte(opacity);
  record.Byte(0);
  record.Ramp(ramp);
  record.Box(from, to);
  Filling(record.Bytes());
}

void SpoolWriter::Record(std::size_t band, std::span<const std::uint8_t> record) noexcept {
  if (band_[band].used + record.size() > kChunkBytes) {
    Flush(band);
    if (!recording_) {
      return;
    }
  }
  Band& kept = band_[band];
  std::copy(record.begin(), record.end(), buffers_.subspan((band * kChunkBytes) + kept.used, record.size()).begin());
  kept.used = static_cast<std::uint16_t>(kept.used + record.size());
}

void SpoolWriter::Flush(std::size_t band) noexcept {
  Band& kept = band_[band];
  if (kept.used == 0 || status_ != SpoolStatus::kOk) {
    return;
  }
  if (chunks_ >= links_.size()) {
    Fail(SpoolStatus::kNoRoom);
    return;
  }
  const std::span<std::uint8_t> chunk = buffers_.subspan(band * kChunkBytes, kChunkBytes);
  std::fill(chunk.begin() + kept.used, chunk.end(), std::uint8_t{kEnd});
  if (!store_->Append(chunk)) {
    Fail(SpoolStatus::kStoreFailed);
    return;
  }
  const auto number = static_cast<std::uint16_t>(chunks_);
  links_[number] = kNone;
  if (kept.tail == kNone) {
    kept.head = number;
  } else {
    links_[kept.tail] = number;
  }
  kept.tail = number;
  kept.used = 0;
  ++chunks_;
}

bool SpoolWriter::BeginBand(std::size_t band, std::span<std::uint8_t> chunk) noexcept {
  if (!finished_ || status_ != SpoolStatus::kOk || band >= bands_ || chunk.size() < kChunkBytes) {
    return false;
  }
  chunk_ = chunk.first(kChunkBytes);
  next_ = band_[band].head;
  at_ = 0;
  loaded_ = false;
  replayHasEnd_ = false;
  replayEnd_ = {};
  return true;
}

bool SpoolWriter::ReplayStep(Filler& filler) noexcept {
  if (status_ != SpoolStatus::kOk) {
    return false;
  }
  if (!loaded_) {
    if (next_ == kNone) {
      return false;
    }
    if (next_ >= chunks_ || !store_->ReadAt(std::uint32_t{next_} * kChunkBytes, chunk_)) {
      Fail(next_ >= chunks_ ? SpoolStatus::kBadRecord : SpoolStatus::kStoreFailed);
      return false;
    }
    next_ = links_[next_];
    at_ = 0;
    loaded_ = true;
  }
  while (at_ < chunk_.size() && chunk_[at_] != kEnd) {
    const std::uint8_t tag = chunk_[at_];
    const std::size_t size = RecordBytes(tag);
    Reading reading(chunk_, at_ + 1);
    if (size == 0 || !reading.Fits(size - 1)) {
      Fail(SpoolStatus::kBadRecord);
      return false;
    }
    at_ += size;
    if (tag == kLine || tag == kLineOn || tag == kLineOnFar) {
      Place from = replayEnd_;
      Place to;
      if (tag == kLine) {
        const Point a = reading.Place();
        from = {.x = a.x, .y = a.y};
      } else if (!replayHasEnd_) {
        Fail(SpoolStatus::kBadRecord);
        return false;
      }
      if (tag == kLineOn) {
        const std::int32_t dx = reading.Two();
        const std::int32_t dy = reading.Two();
        to = {.x = static_cast<std::int32_t>(std::int64_t{from.x} + dx),
              .y = static_cast<std::int32_t>(std::int64_t{from.y} + dy)};
      } else {
        const Point b = reading.Place();
        to = {.x = b.x, .y = b.y};
      }
      filler.AddPlaced(from, to);
      replayHasEnd_ = true;
      replayEnd_ = to;
      continue;
    }
    // A filling: the last thing this call does.
    const auto rule = static_cast<FillRule>(reading.Byte());
    if (rule != FillRule::kNonZero && rule != FillRule::kEvenOdd) {
      Fail(SpoolStatus::kBadRecord);
      return false;
    }
    if (tag == kFill || tag == kFillBox) {
      const std::uint8_t gray = reading.Byte();
      const std::uint8_t opacity = reading.Byte();
      if (tag == kFill) {
        filler.Fill(rule, gray, opacity);
      } else {
        const Point from = reading.Place();
        filler.Fill(rule, gray, opacity, from, reading.Place());
      }
    } else {
      const std::uint8_t opacity = reading.Byte();
      static_cast<void>(reading.Byte());
      GrayRamp ramp;
      if (!reading.Ramp(ramp)) {
        Fail(SpoolStatus::kBadRecord);
        return false;
      }
      if (tag == kFillRamp) {
        filler.Fill(rule, ramp, opacity);
      } else {
        const Point from = reading.Place();
        filler.Fill(rule, ramp, opacity, from, reading.Place());
      }
    }
    return true;
  }
  // The chunk is all given back.
  loaded_ = false;
  return next_ != kNone;
}

}  // namespace bn::draw
