#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "core/byte_source.h"

// What the fuzz targets share.

namespace bn::fuzz {

// Ends the program, saying what was wrong and where, unless `holds`. This
// is how a target reports something that no sanitizer would see: the
// engine keeps the input that led here, and the program that goes through
// the kept inputs fails.
inline void Check(bool holds, const char* file, int line, const char* condition, const char* what) noexcept {
  if (!holds) {
    std::fprintf(stderr, "%s:%d: %s (%s)\n", file, line, what, condition);
    std::abort();
  }
}

// A macro, for the place and the text of the condition.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define BN_FUZZ_CHECK(condition, what) ::bn::fuzz::Check((condition), __FILE__, __LINE__, #condition, what)

// The bytes a target was given, taken from the front a few at a time. A
// target reads its choices (how big a piece to read by, how much memory to
// give) from the first bytes and what it is testing from the rest. Past
// the end there are only zeros, so an input of any length, none included,
// is one the target can take.
class Input {
 public:
  Input(const std::uint8_t* data, std::size_t size) noexcept : bytes_(data, size) {}

  std::uint8_t Byte() noexcept {
    if (bytes_.empty()) {
      return 0;
    }
    const std::uint8_t byte = bytes_.front();
    bytes_ = bytes_.subspan(1);
    return byte;
  }

  // A number of two bytes, the low one first.
  std::uint32_t Number16() noexcept {
    const std::uint32_t low = Byte();
    return low | (std::uint32_t{Byte()} << 8U);
  }

  // The next `count` bytes, or as many as there are.
  std::span<const std::uint8_t> Take(std::size_t count) noexcept {
    const std::span<const std::uint8_t> taken = bytes_.first(std::min(count, bytes_.size()));
    bytes_ = bytes_.subspan(taken.size());
    return taken;
  }

  // Everything that is left.
  std::span<const std::uint8_t> Rest() noexcept { return Take(bytes_.size()); }
  // Nothing is left.
  bool Empty() const noexcept { return bytes_.empty(); }

 private:
  std::span<const std::uint8_t> bytes_;
};

// A source over memory that gives at most so many bytes at a call, and
// counts what was asked of it.
class PiecewiseSource final : public core::ByteSource {
 public:
  PiecewiseSource(std::span<const std::uint8_t> bytes, std::size_t piece) noexcept
      : bytes_(bytes), piece_(std::max<std::size_t>(piece, 1)) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    ++calls_;
    const std::size_t count = std::min({dst.size(), piece_, bytes_.size() - taken_});
    if (count != 0) {
      std::memcpy(dst.data(), bytes_.data() + taken_, count);
    }
    taken_ += count;
    return static_cast<std::ptrdiff_t>(count);
  }

  // How many bytes have been taken from it, and in how many calls.
  std::size_t Taken() const noexcept { return taken_; }
  std::size_t Calls() const noexcept { return calls_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t piece_;
  std::size_t taken_ = 0;
  std::size_t calls_ = 0;
};

// Whether `text` is made of whole UTF-8 characters: each first byte is
// followed by as many continuing bytes as it calls for, and there is no
// continuing byte without one. (Which characters they are is not looked
// at.)
inline bool IsWholeCharacters(std::string_view text) noexcept {
  std::size_t at = 0;
  while (at < text.size()) {
    const auto first = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    if (first < 0x80U) {
      length = 1;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
    } else {
      return false;
    }
    if (text.size() - at < length) {
      return false;
    }
    for (std::size_t i = 1; i < length; ++i) {
      if ((static_cast<unsigned char>(text[at + i]) & 0xC0U) != 0x80U) {
        return false;
      }
    }
    at += length;
  }
  return true;
}

// Reads every byte of `text`, so that a sanitizer sees text that is not
// all there. The sum is of no interest but to stop the reading being
// optimised away.
inline std::uint32_t Touch(std::string_view text) noexcept {
  std::uint32_t sum = 0;
  for (const char ch : text) {
    sum += static_cast<unsigned char>(ch);
  }
  return sum;
}

}  // namespace bn::fuzz
