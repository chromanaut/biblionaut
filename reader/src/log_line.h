#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/trace_stream.h"

// What the device checks write their lines of log text with.

namespace bn::reader {

// A line of text put together piece by piece. What does not fit is left
// off the end.
class LogLine {
 public:
  LogLine& Text(std::string_view text) noexcept {
    for (const char ch : text) {
      Put(ch);
    }
    return *this;
  }
  LogLine& Number(std::uint64_t value) noexcept {
    std::array<char, 20> digits{};
    std::size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0);
    while (count > 0) {
      Put(digits[--count]);
    }
    return *this;
  }
  // Eight digits of hexadecimal, as checksums are written.
  LogLine& Hex8(std::uint32_t value) noexcept {
    constexpr std::string_view kDigits = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
      Put(kDigits[(value >> static_cast<unsigned>(shift)) & 0xFU]);
    }
    return *this;
  }
  // A number and what it counts: "1 read", "2 reads".
  LogLine& Count(std::uint64_t value, std::string_view one, std::string_view many) noexcept {
    return Number(value).Text(value == 1 ? one : many);
  }
  std::string_view View() const noexcept { return {text_.data(), length_}; }
  // Starts another line in the same place.
  void Clear() noexcept { length_ = 0; }

 private:
  void Put(char ch) noexcept {
    if (length_ < text_.size()) {
      text_[length_++] = ch;
    }
  }

  std::array<char, core::kMaxTextLength> text_{};
  std::size_t length_ = 0;
};

// KB (of 1024 bytes) a second; 0 if no time passed.
inline std::uint64_t KbPerSecond(std::uint64_t bytes, std::uint64_t microseconds) noexcept {
  return microseconds == 0 ? 0 : (bytes * 1'000'000) / microseconds / 1024;
}

inline std::uint32_t Clamped(std::uint64_t value) noexcept {
  return value > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(value);
}

}  // namespace bn::reader
