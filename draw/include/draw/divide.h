#pragma once

#include <bit>
#include <cstdint>

// Dividing many numbers by the same one, by multiplying.
//
// The device's processor takes many cycles over a division and few over a
// multiplication. Where every pixel is divided by the same number, that
// number is turned once into a multiplier and two shifts, and each division
// is then a multiplication, a subtraction, an addition and two shifts. The
// quotient is exactly what `/` gives, rounded down, for every 32-bit number
// divided by every 32-bit divisor but 0.
//
// The method is Torbjörn Granlund and Peter L. Montgomery's, "Division by
// Invariant Integers using Multiplication" (PLDI 1994), as their figure 4.1
// sets it out for unsigned numbers of 32 bits.

namespace bn::draw {

class Divider {
 public:
  // Divides by 1.
  constexpr Divider() noexcept = default;

  // Divides by `divisor`, which is not 0.
  explicit constexpr Divider(std::uint32_t divisor) noexcept {
    // The divisor's bits, rounded up: 2^(bits - 1) < divisor <= 2^bits.
    const std::uint32_t bits = divisor <= 1 ? 0U : 32U - static_cast<std::uint32_t>(std::countl_zero(divisor - 1U));
    // 2^32 * (2^bits - divisor) / divisor, rounded down, and one more: less
    // than 2^32, since 2^bits is less than twice the divisor.
    const std::uint64_t over = (std::uint64_t{1} << bits) - divisor;
    multiplier_ = static_cast<std::uint32_t>(((over << 32U) / divisor) + 1U);
    firstShift_ = bits == 0 ? 0U : 1U;
    secondShift_ = bits == 0 ? 0U : bits - 1U;
  }

  // `number` divided by the divisor, rounded down.
  constexpr std::uint32_t Divide(std::uint32_t number) const noexcept {
    const auto high = static_cast<std::uint32_t>((std::uint64_t{number} * multiplier_) >> 32U);
    return (high + ((number - high) >> firstShift_)) >> secondShift_;
  }

 private:
  std::uint32_t multiplier_ = 1;
  std::uint32_t firstShift_ = 0;
  std::uint32_t secondShift_ = 0;
};

}  // namespace bn::draw
