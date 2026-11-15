#include "draw/svg_number.h"

#include <array>

namespace bn::draw {

namespace {

// The largest a Number is let be: 2^30, in its 1/2^32ths.
constexpr std::uint64_t kLargest = 4611686018427387904;

// The largest the part before the point is let be: 2^30.
constexpr std::uint64_t kLargestWhole = 1073741824;

// What a 1 is worth in each place after the point, in 1/2^48ths: a tenth, a
// hundredth and so on. Twelve places are more than a Number can show.
constexpr std::array<std::uint64_t, 12> kPlaces = {28147497671066, 2814749767107, 281474976711, 28147497671,
                                                   2814749767,     281474977,     28147498,     2814750,
                                                   281475,         28147,         2815,         281};

constexpr std::uint16_t kExponentLimit = 1000;

bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

bool SvgNumber::TakeExponentMark(char c) noexcept {
  if (c == 'e' || c == 'E') {
    stage_ = Stage::kExponentMark;
    return true;
  }
  return false;
}

bool SvgNumber::Take(char c) noexcept {
  const bool digit = IsDigit(c);
  const auto value = static_cast<std::uint32_t>(digit ? c - '0' : 0);
  const auto addWhole = [this, value]() noexcept {
    const std::uint64_t next = (std::uint64_t{whole_} * 10) + value;
    if (next > kLargestWhole) {
      wholeTooLarge_ = true;
    } else {
      whole_ = static_cast<std::uint32_t>(next);
    }
  };
  const auto addFraction = [this, value]() noexcept {
    if (fractionDigits_ < kPlaces.size()) {
      fraction_ += value * kPlaces[fractionDigits_];
      ++fractionDigits_;
    }
  };
  const auto addExponent = [this, value]() noexcept {
    if (exponent_ < kExponentLimit) {
      exponent_ = static_cast<std::uint16_t>((exponent_ * 10U) + value);
    }
  };

  switch (stage_) {
    case Stage::kStart:
      if (c == '+' || c == '-') {
        negative_ = c == '-';
        stage_ = Stage::kSign;
        return true;
      }
      [[fallthrough]];
    case Stage::kSign:
      if (digit) {
        addWhole();
        stage_ = Stage::kWhole;
        return true;
      }
      if (c == '.') {
        stage_ = Stage::kLonePoint;
        return true;
      }
      return false;
    case Stage::kWhole:
      if (digit) {
        addWhole();
        return true;
      }
      if (c == '.') {
        stage_ = Stage::kPoint;
        return true;
      }
      return TakeExponentMark(c);
    case Stage::kPoint:
    case Stage::kFraction:
      if (digit) {
        addFraction();
        stage_ = Stage::kFraction;
        return true;
      }
      return TakeExponentMark(c);
    case Stage::kLonePoint:
      if (digit) {
        addFraction();
        stage_ = Stage::kFraction;
        return true;
      }
      return false;
    case Stage::kExponentMark:
      if (c == '+' || c == '-') {
        exponentNegative_ = c == '-';
        stage_ = Stage::kExponentSign;
        return true;
      }
      [[fallthrough]];
    case Stage::kExponentSign:
    case Stage::kExponent:
      if (digit) {
        addExponent();
        stage_ = Stage::kExponent;
        return true;
      }
      return false;
  }
  return false;
}

bool SvgNumber::Valid() const noexcept {
  return stage_ == Stage::kWhole || stage_ == Stage::kPoint || stage_ == Stage::kFraction || stage_ == Stage::kExponent;
}

Number SvgNumber::Value(bool& clamped) const noexcept {
  constexpr std::uint64_t kHalf = 32768;
  std::uint64_t size = (static_cast<std::uint64_t>(whole_) << 32U) + ((fraction_ + kHalf) >> 16U);
  if (wholeTooLarge_ || size > kLargest) {
    size = kLargest;
    clamped = true;
  }
  if (exponentNegative_) {
    for (std::uint16_t times = 0; times < exponent_ && size != 0; ++times) {
      size /= 10;
    }
  } else {
    for (std::uint16_t times = 0; times < exponent_ && size != 0; ++times) {
      if (size > kLargest / 10) {
        size = kLargest;
        clamped = true;
        break;
      }
      size *= 10;
    }
  }
  const auto number = static_cast<Number>(size);
  return negative_ ? -number : number;
}

Fixed FixedOf(Number number, bool& clamped) noexcept {
  constexpr int kShift = 32 - kFixedBits;
  constexpr Number kHalf = 524288;
  return Clamp32(ShiftDown(number + kHalf, kShift), clamped);
}

Coefficient CoefficientOf(Number number, bool& clamped) noexcept {
  constexpr int kShift = 32 - kCoefficientBits;
  constexpr Number kHalf = 2048;
  return Clamp32(ShiftDown(number + kHalf, kShift), clamped);
}

Angle AngleOfDegrees(Number number) noexcept {
  // A turn is 360 degrees and 2^32 of an Angle, and the number is in
  // 1/2^32ths of a degree: so the angle is the number over 360, and what
  // does not fit in 32 bits is whole turns.
  return static_cast<Angle>(static_cast<std::uint64_t>(number / 360));
}

bool ScanNumber(std::string_view text, std::size_t& at, Number& number, bool& clamped) noexcept {
  SvgNumber reader;
  std::size_t end = at;
  while (end < text.size() && reader.Take(text[end])) {
    ++end;
  }
  if (!reader.Valid()) {
    return false;
  }
  number = reader.Value(clamped);
  at = end;
  return true;
}

}  // namespace bn::draw
