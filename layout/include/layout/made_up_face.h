#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "layout/paragraph_setter.h"

// A face with made-up widths, to set text in until there are fonts to
// measure it with. `bn-lines` sets text in it, and so does the device check
// for layout, so that the lines a device sets can be set beside the
// desktop's.
//
// Every character has a width that depends on its first byte and on
// nothing else, so that whoever reads a line can add it up. Widths are in
// quarters of a pixel:
//
//   - a small letter is 36;
//   - i, l, j, t, f and . , ; : ' ! | are 18;
//   - m, w, M and W are 54;
//   - any other capital is 44;
//   - a hyphen-minus is 22;
//   - a character of more than one byte is 40, but for a mark set over the
//     letter before it (U+0300 to U+036F), which has no width;
//   - a space is 18, and the hyphen shown at a line's end 22.
//
// In each style after the first, everything is 4 wider than in the one
// before.
//
// Measuring costs a look at each byte, which is less than a real face's
// measuring will cost.

namespace bn::layout {

class MadeUpFace final : public TextMeasure {
 public:
  // The width of the character whose first byte is `first`; 0 for a byte
  // that goes on with a character, and for the first byte of a mark.
  static std::uint16_t CharacterWidth(char first, std::uint16_t style) noexcept;

  void Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
               std::span<std::uint16_t> widths, std::span<std::uint16_t> hyphens) noexcept override;
  std::uint16_t SpaceWidth(std::uint16_t style) noexcept override;
  // The hyphen shown at a line's end, which is as wide after any piece.
  static std::uint16_t HyphenWidth(std::uint16_t style) noexcept;

  // How many bytes it has been given to measure, over all its calls.
  std::uint64_t BytesMeasured() const noexcept { return bytesMeasured_; }

 private:
  std::uint64_t bytesMeasured_ = 0;
};

}  // namespace bn::layout
