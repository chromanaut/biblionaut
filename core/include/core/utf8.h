#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Reading UTF-8 a character at a time, strictly.

namespace bn::core {

// The largest code point there is.
inline constexpr std::uint32_t kLargestCodePoint = 0x10FFFF;

// One character of some text.
struct Character {
  std::uint32_t codePoint = 0;
  std::size_t bytes = 1;
  bool good = false;  // It is a character. If not, it is one byte that is none.
};

// The character that begins at `text[at]`, which has to be inside `text`,
// read strictly: nothing written the long way round, no half of a pair,
// nothing past the last code point. A byte that does not begin a character
// so read is one byte that is none, and the next is looked at after it.
constexpr Character CharacterAt(std::string_view text, std::size_t at) noexcept {
  const auto lead = static_cast<std::uint8_t>(text[at]);
  if (lead < 0x80) {
    return {.codePoint = lead, .bytes = 1, .good = true};
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t least = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
    least = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
    least = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
    least = 0x10000;
  } else {
    return {};
  }
  if (text.size() - at < length) {
    return {};
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<std::uint8_t>(text[at + i]);
    if ((next & 0xC0U) != 0x80U) {
      return {};
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  if (value < least || value > kLargestCodePoint || (value >= 0xD800 && value <= 0xDFFF)) {
    return {};
  }
  return {.codePoint = value, .bytes = length, .good = true};
}

}  // namespace bn::core
