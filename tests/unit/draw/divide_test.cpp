#include "draw/divide.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

// A Divider is held to `/` itself: every quotient has to be the same.

namespace {

using bn::draw::Divider;

constexpr std::uint32_t kLargest = std::numeric_limits<std::uint32_t>::max();

std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// The numbers where a quotient changes, or would if it were off: 0, the
// largest, and each side of a few multiples of the divisor, the last that
// fits among them.
std::vector<std::uint32_t> NumbersFor(std::uint32_t divisor) {
  std::vector<std::uint32_t> numbers = {0, 1, kLargest, kLargest - 1};
  const std::uint64_t last = std::uint64_t{kLargest} / divisor * divisor;
  for (const std::uint64_t multiple : {std::uint64_t{divisor}, std::uint64_t{divisor} * 2, std::uint64_t{divisor} * 255,
                                       std::uint64_t{divisor} * 65535, last}) {
    for (const std::int64_t side : {-1, 0, 1}) {
      const std::int64_t number = static_cast<std::int64_t>(multiple) + side;
      if (number >= 0 && number <= kLargest) {
        numbers.push_back(static_cast<std::uint32_t>(number));
      }
    }
  }
  return numbers;
}

TEST(Divider, DividesByOneUntilToldOtherwise) {
  constexpr Divider kByOne;
  static_assert(kByOne.Divide(0) == 0);
  static_assert(kByOne.Divide(kLargest) == kLargest);
  EXPECT_EQ(kByOne.Divide(12345), 12345U);
}

TEST(Divider, CanBeWorkedOutAsTheProgramIsCompiled) {
  static_assert(Divider(3).Divide(kLargest) == kLargest / 3);
  static_assert(Divider(7).Divide(100) == 14);
  static_assert(Divider(kLargest).Divide(kLargest) == 1);
  static_assert(Divider(kLargest).Divide(kLargest - 1) == 0);
}

TEST(Divider, GivesWhatDivisionGivesForEveryDivisorUpTo70000) {
  // Past the scaler's largest side, at the numbers where a quotient
  // changes.
  for (std::uint32_t divisor = 1; divisor <= 70000; ++divisor) {
    const Divider by(divisor);
    for (const std::uint32_t number : NumbersFor(divisor)) {
      ASSERT_EQ(by.Divide(number), number / divisor) << number << " / " << divisor;
    }
  }
}

TEST(Divider, GivesWhatDivisionGivesForEveryNumberBySomeDivisors) {
  // Every 32-bit number would take too long: every number up to 2^22, and a
  // step of a prime through the rest.
  for (const std::uint32_t divisor : {3U, 7U, 641U, 4800U, 65535U, 131070U, 16776960U, 2147483649U}) {
    const Divider by(divisor);
    for (std::uint32_t number = 0; number < (1U << 22U); ++number) {
      ASSERT_EQ(by.Divide(number), number / divisor) << number << " / " << divisor;
    }
    for (std::uint64_t number = 1U << 22U; number <= kLargest; number += 4093) {
      const auto n = static_cast<std::uint32_t>(number);
      ASSERT_EQ(by.Divide(n), n / divisor) << n << " / " << divisor;
    }
  }
}

TEST(Divider, GivesWhatDivisionGivesForPowersOfTwoAndTheirNeighbours) {
  for (std::uint32_t bit = 0; bit < 32; ++bit) {
    const std::uint32_t power = 1U << bit;
    for (const std::uint32_t divisor : {power - 1, power, power + 1}) {
      if (divisor == 0) {
        continue;
      }
      const Divider by(divisor);
      for (const std::uint32_t number : NumbersFor(divisor)) {
        ASSERT_EQ(by.Divide(number), number / divisor) << number << " / " << divisor;
      }
    }
  }
}

TEST(Divider, GivesWhatDivisionGivesForNumbersAndDivisorsAtRandom) {
  std::mt19937 random = Repeatable(20261117);
  std::uniform_int_distribution<std::uint32_t> any(1, kLargest);
  for (std::uint32_t round = 0; round < 20000; ++round) {
    // Divisors of every size, not only the large ones most numbers are.
    const std::uint32_t divisor = std::max<std::uint32_t>(1, any(random) >> (round % 32U));
    const Divider by(divisor);
    for (int at = 0; at < 50; ++at) {
      const std::uint32_t number = any(random);
      ASSERT_EQ(by.Divide(number), number / divisor) << number << " / " << divisor;
    }
  }
}

}  // namespace
