#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "layout/paragraph_setter.h"

// A measure of text for tests, in place of a font: every character has a
// width that can be worked out from its first byte and nothing else, so a
// test can say how wide any text is by adding up. It also keeps count of
// what it was asked, and of anything it was given that it should not have
// been.

namespace bn::test {

class TestMeasure final : public layout::TextMeasure {
 public:
  // With `each` every character is that wide, and so are a space and a
  // hyphen. With 0 the characters are the widths of a made-up face, in
  // which letters differ as they do in print.
  explicit TestMeasure(std::uint16_t each = 0) noexcept : each_(each) {}

  // Added to every character, space and hyphen for each step of the style's
  // number: text in style 2 is this much wider a character, twice over.
  std::uint16_t widerByStyle = 0;
  // Everything is 0 wide, whatever it is: any amount of it fits on a line.
  bool nothingHasWidth = false;

  // What it has been asked: how many times for the widths of pieces, and
  // over how many bytes of text in all.
  std::size_t calls = 0;
  std::size_t bytes = 0;
  // It was given a soft hyphen or a zero width space to measure, which are
  // on no line; or a piece that begins or ends inside a character.
  bool measuredAMark = false;
  bool cutInsideACharacter = false;
  // It was asked for pieces that do not end in order, or not where the text
  // does.
  bool badPieces = false;

  // How wide one character is: `first` is its first byte. The bytes that
  // go on with a character, and a mark set over the letter before it, add
  // nothing.
  std::uint16_t CharacterWidth(char first, std::uint16_t style) const noexcept {
    const auto byte = static_cast<std::uint8_t>(first);
    if (nothingHasWidth || (byte & 0xC0U) == 0x80U || byte == 0xCC || byte == 0xCD) {
      return 0;
    }
    const auto more = static_cast<std::uint16_t>(widerByStyle * style);
    if (each_ != 0) {
      return static_cast<std::uint16_t>(each_ + more);
    }
    const std::string_view narrow = "iljtf.,;:'!|";
    const std::string_view wide = "mwMW";
    std::uint16_t width = 36;
    if (narrow.find(first) != std::string_view::npos) {
      width = 18;
    } else if (wide.find(first) != std::string_view::npos) {
      width = 54;
    } else if (first >= 'A' && first <= 'Z') {
      width = 44;
    } else if (first == '-') {
      width = 22;
    } else if (byte >= 0xC0) {
      width = 40;
    }
    return static_cast<std::uint16_t>(width + more);
  }

  // How wide `text` is: its characters' widths added up.
  std::uint32_t WidthOf(std::string_view text, std::uint16_t style) const noexcept {
    std::uint32_t width = 0;
    for (const char ch : text) {
      width += CharacterWidth(ch, style);
    }
    return width;
  }

  void Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
               std::span<std::uint16_t> widths) noexcept override {
    ++calls;
    bytes += text.size();
    if (text.find("\xC2\xAD") != std::string_view::npos || text.find("\xE2\x80\x8B") != std::string_view::npos) {
      measuredAMark = true;
    }
    if (ends.empty() || ends.size() != widths.size() || ends.back() != text.size()) {
      badPieces = true;
      return;
    }
    std::size_t from = 0;
    for (std::size_t i = 0; i < ends.size(); ++i) {
      const std::size_t end = ends[i];
      if (end <= from || end > text.size()) {
        badPieces = true;
        return;
      }
      if ((static_cast<std::uint8_t>(text[from]) & 0xC0U) == 0x80U) {
        cutInsideACharacter = true;
      }
      widths[i] = static_cast<std::uint16_t>(WidthOf(text.substr(from, end - from), style));
      from = end;
    }
  }

  std::uint16_t SpaceWidth(std::uint16_t style) noexcept override {
    return nothingHasWidth ? 0 : static_cast<std::uint16_t>((each_ != 0 ? each_ : 18) + (widerByStyle * style));
  }

  std::uint16_t HyphenWidth(std::uint16_t style) noexcept override {
    return nothingHasWidth ? 0 : static_cast<std::uint16_t>((each_ != 0 ? each_ : 22) + (widerByStyle * style));
  }

 private:
  std::uint16_t each_;
};

}  // namespace bn::test
