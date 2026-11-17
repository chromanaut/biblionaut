#include "draw/scale.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::draw {

namespace {

// How a blend between two old pixels is weighed: in 4096ths of the way from
// one to the next. Finer would not fit the sums of a blend down a column in
// 32 bits; 256ths left a pixel made larger both ways up to one and a half
// grays from where it should be.
constexpr std::uint32_t kWeightOne = 4096;

}  // namespace

const char* ScaleStatusName(ScaleStatus status) noexcept {
  switch (status) {
    case ScaleStatus::kOk:
      return "ok";
    case ScaleStatus::kNotBegun:
      return "not begun";
    case ScaleStatus::kBadSize:
      return "bad size";
    case ScaleStatus::kNoRoom:
      return "no room";
    case ScaleStatus::kOutOfTurn:
      return "out of turn";
  }
  return "unknown";
}

// ---- Where new pixels lie over old ones, made larger ----

void Scaler::Blend::Begin(std::uint32_t oldSize, std::uint32_t newSize) noexcept {
  // The middle of new pixel x lies at (x + 1/2) * old / new - 1/2 in old
  // pixels, which is ((2x + 1) * old - new) / (2 * new). For the first, that
  // is before the first old pixel's middle (old < new): one before it, and
  // (old + new) over twice new of the way on.
  oldSize_ = oldSize;
  newSize_ = newSize;
  before_ = -1;
  remainder_ = oldSize + newSize;
}

void Scaler::Blend::Next() noexcept {
  // One new pixel on is 2 * old over 2 * new further, which is less than one
  // old pixel.
  remainder_ += 2U * oldSize_;
  if (remainder_ >= 2U * newSize_) {
    remainder_ -= 2U * newSize_;
    ++before_;
  }
}

std::uint32_t Scaler::Blend::Weight() const noexcept {
  return ((remainder_ * kWeightOne) + newSize_) / (2U * newSize_);
}

// ---- An old row, made the new width ----

void Scaler::Across::Begin(std::span<const std::uint8_t> gray, std::uint32_t newWidth) noexcept {
  gray_ = gray;
  newWidth_ = newWidth;
  at_ = 0;
  used_ = 0;
  if (gray.size() < newWidth) {
    blend_.Begin(static_cast<std::uint32_t>(gray.size()), newWidth);
  }
}

std::uint32_t Scaler::Across::Next() noexcept {
  const auto oldWidth = static_cast<std::uint32_t>(gray_.size());
  if (oldWidth == newWidth_) {
    return std::uint32_t{gray_[at_++]} << 8U;
  }
  if (oldWidth < newWidth_) {
    const std::int32_t before = blend_.Before();
    const std::uint32_t weight = blend_.Weight();
    blend_.Next();
    const std::uint32_t left = gray_[before < 0 ? 0U : static_cast<std::uint32_t>(before)];
    const std::uint32_t right = gray_[std::min(static_cast<std::uint32_t>(before + 1), oldWidth - 1U)];
    // In 4096ths of a gray, rounded to 256ths.
    return ((left * (kWeightOne - weight)) + (right * weight) + 8U) >> 4U;
  }
  // Smaller. A new pixel covers `oldWidth` units and an old one `newWidth_`:
  // the old pixels it covers are added up, each by how many of its units it
  // covers, and the sum divided by the new pixel's units. `used_` is how
  // many units of old pixel `at_` earlier new pixels have had.
  std::uint32_t sum = 0;
  std::uint32_t left = oldWidth;
  while (left > 0) {
    const std::uint32_t take = std::min(newWidth_ - used_, left);
    sum += gray_[at_] * take;
    left -= take;
    used_ += take;
    if (used_ == newWidth_) {
      ++at_;
      used_ = 0;
    }
  }
  return ((sum * 256U) + (oldWidth / 2U)) / oldWidth;
}

// ---- The scaler ----

Scaler::Scaler(std::span<std::uint32_t> sums, std::span<std::uint8_t> row) noexcept : sums_(sums), row_(row) {}

ScaleStatus Scaler::Begin(std::uint32_t oldWidth, std::uint32_t oldHeight, std::uint32_t newWidth,
                          std::uint32_t newHeight, RowSink& out) noexcept {
  out_ = &out;
  taken_ = 0;
  given_ = 0;
  current_ = 0;
  filled_ = 0;
  waiting_ = false;
  const auto allowed = [](std::uint32_t side) { return side >= 1 && side <= kMaxSide; };
  if (!allowed(oldWidth) || !allowed(oldHeight) || !allowed(newWidth) || !allowed(newHeight)) {
    status_ = ScaleStatus::kBadSize;
    return status_;
  }
  if (sums_.size() < SumsFor(newWidth) || row_.size() < newWidth) {
    status_ = ScaleStatus::kNoRoom;
    return status_;
  }
  oldWidth_ = oldWidth;
  oldHeight_ = oldHeight;
  newWidth_ = newWidth;
  newHeight_ = newHeight;
  if (newHeight == oldHeight) {
    vertical_ = Vertical::kSame;
  } else if (newHeight < oldHeight) {
    vertical_ = Vertical::kSmaller;
    std::fill_n(sums_.begin(), SumsFor(newWidth), 0U);
  } else {
    vertical_ = Vertical::kLarger;
    down_.Begin(oldHeight, newHeight);
  }
  status_ = ScaleStatus::kOk;
  return status_;
}

bool Scaler::Ready() const noexcept {
  if (status_ != ScaleStatus::kOk || given_ == newHeight_) {
    return false;
  }
  switch (vertical_) {
    case Vertical::kSame:
      return taken_ > given_;
    case Vertical::kSmaller:
      return waiting_;
    case Vertical::kLarger: {
      // The new row needs the old rows on either side of its middle: the
      // later of them has to have been taken.
      const std::int32_t later = std::min<std::int32_t>(down_.Before() + 1, static_cast<std::int32_t>(oldHeight_ - 1U));
      return static_cast<std::int32_t>(taken_) > std::max<std::int32_t>(later, 0);
    }
  }
  return false;
}

void Scaler::Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept {
  if (status_ != ScaleStatus::kOk) {
    return;
  }
  if (y != taken_ || y >= oldHeight_ || gray.size() != oldWidth_ || Ready()) {
    Fail(ScaleStatus::kOutOfTurn);
    return;
  }
  across_.Begin(gray, newWidth_);
  const std::span<std::uint32_t> first = sums_.first(newWidth_);
  const std::span<std::uint32_t> second = sums_.subspan(newWidth_, newWidth_);
  switch (vertical_) {
    case Vertical::kSame:
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        first[x] = across_.Next();
      }
      break;
    case Vertical::kSmaller: {
      // A new row covers `oldHeight_` units and an old one `newHeight_`. This
      // one's units go to the new row being made, and what is left of them,
      // if it ends that row, to the next.
      const std::uint32_t take = std::min(newHeight_, oldHeight_ - filled_);
      const std::uint32_t rest = newHeight_ - take;
      const std::span<std::uint32_t> making = current_ == 0 ? first : second;
      const std::span<std::uint32_t> next = current_ == 0 ? second : first;
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        const std::uint32_t pixel = across_.Next();
        making[x] += pixel * take;
        next[x] += pixel * rest;
      }
      filled_ += take;
      if (filled_ == oldHeight_) {
        // That new row is whole, and waits to be given out; the next has
        // what was left of this old row.
        waiting_ = true;
        current_ ^= 1U;
        filled_ = rest;
      }
      break;
    }
    case Vertical::kLarger: {
      // Kept by the row's number, odd or even: the two last taken.
      const std::span<std::uint32_t> into = (y & 1U) == 0 ? first : second;
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        into[x] = across_.Next();
      }
      break;
    }
  }
  ++taken_;
}

bool Scaler::Step() noexcept {
  if (!Ready()) {
    return false;
  }
  const std::span<std::uint32_t> first = sums_.first(newWidth_);
  const std::span<std::uint32_t> second = sums_.subspan(newWidth_, newWidth_);
  const std::span<std::uint8_t> row = row_.first(newWidth_);
  switch (vertical_) {
    case Vertical::kSame:
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        row[x] = static_cast<std::uint8_t>((first[x] + 128U) >> 8U);
      }
      break;
    case Vertical::kSmaller: {
      // The whole row is the one that is not being made; it is emptied for
      // the row after next.
      const std::span<std::uint32_t> whole = current_ == 0 ? second : first;
      const std::uint32_t units = oldHeight_ * 256U;
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        row[x] = static_cast<std::uint8_t>((whole[x] + (units / 2U)) / units);
        whole[x] = 0;
      }
      waiting_ = false;
      break;
    }
    case Vertical::kLarger: {
      const std::int32_t before = down_.Before();
      const std::uint32_t weight = down_.Weight();
      const std::uint32_t upper = before < 0 ? 0U : static_cast<std::uint32_t>(before);
      const std::uint32_t lower = std::min(static_cast<std::uint32_t>(before + 1), oldHeight_ - 1U);
      const std::span<std::uint32_t> above = (upper & 1U) == 0 ? first : second;
      const std::span<std::uint32_t> below = (lower & 1U) == 0 ? first : second;
      // 256ths of a gray by 4096ths of the way: the sum is in 1,048,576ths.
      for (std::uint32_t x = 0; x < newWidth_; ++x) {
        row[x] = static_cast<std::uint8_t>(((above[x] * (kWeightOne - weight)) + (below[x] * weight) + 524288U) >> 20U);
      }
      down_.Next();
      break;
    }
  }
  out_->Row(given_, row);
  ++given_;
  return true;
}

void Scaler::Fail(ScaleStatus status) noexcept { status_ = status; }

}  // namespace bn::draw
