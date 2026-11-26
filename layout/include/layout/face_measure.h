#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "draw/font.h"
#include "layout/paragraph_setter.h"

// Text measured in the faces of a fonts image (draw/font.h), for the
// paragraph setter, and the glyphs it is then drawn with.
//
// Each of the setter's style numbers is given a face. A character is drawn
// with its style's face if the face has a glyph for it, with the fallback
// face if that has one, and otherwise with its style's face's glyph for
// what it lacks. A soft hyphen and a zero width space are not drawn and
// take no room.
//
// Every glyph is drawn on a whole pixel, and a glyph is always the same
// number of pixels after the one before it: its advance, which an image has
// in quarters of a pixel, to the nearest pixel (PenStep()). So the same two
// letters are the same distance apart wherever they come on a page. Adding
// the advances up as they are and rounding where the pen has got to would
// not give that: two letters would be a pixel further apart in one place
// than in another, by what was left over before them, and that is plain to
// the eye. There is no kerning yet; when there is, it is added to the
// advance before the rounding.
//
// Widths are in quarters of a pixel, as the setter counts, and each is a
// whole number of pixels: a piece of a word is as wide as its characters'
// steps added up, and a space and a hyphen are rounded as a letter is. So
// what is drawn from the pieces' widths lines up exactly with what was
// measured.
//
// The hyphen shown where a line ends inside a word is the style's U+2010,
// or its hyphen-minus if it has none.

namespace bn::layout {

class FaceMeasure final : public TextMeasure {
 public:
  // The most styles: as many as one word may be in.
  static constexpr std::size_t kMostStyles = ParagraphSetter::kMaxStyles;

  // The face text in `style` is drawn in. A style given none takes style
  // 0's.
  void SetFace(std::uint16_t style, draw::Face face) noexcept;
  void SetFallback(draw::Face face) noexcept { fallback_ = face; }
  draw::Face FaceOf(std::uint16_t style) const noexcept;

  // The glyph `codePoint` is drawn with in `style`, into `glyph`. False for
  // a character that is not drawn, or when there is no glyph to draw at
  // all.
  bool GlyphFor(std::uint16_t style, std::uint32_t codePoint, draw::Glyph& glyph) const noexcept;
  // The glyph of the hyphen shown at a line's end.
  bool HyphenGlyph(std::uint16_t style, draw::Glyph& glyph) const noexcept;

  // What a width is counted in: so many to a pixel.
  static constexpr std::uint16_t kUnitsToAPixel = 4;
  // The most pixels a width can say.
  static constexpr std::uint16_t kMostPixels = 0xFFFF / kUnitsToAPixel;
  // How many pixels the pen moves on after `glyph`: its advance to the
  // nearest pixel, a half going up. Whoever draws what was measured moves
  // the pen by this and by nothing else.
  static constexpr std::uint16_t PenStep(const draw::Glyph& glyph) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(glyph.advance) + (kUnitsToAPixel / 2U)) /
                                      kUnitsToAPixel);
  }

  void Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
               std::span<std::uint16_t> widths) noexcept override;
  std::uint16_t SpaceWidth(std::uint16_t style) noexcept override;
  std::uint16_t HyphenWidth(std::uint16_t style) noexcept override;

  // Whether `codePoint` is drawn at all.
  static bool Drawn(std::uint32_t codePoint) noexcept { return codePoint != 0x00AD && codePoint != 0x200B; }

  // How many bytes it has been given to measure, over all its calls.
  std::uint64_t BytesMeasured() const noexcept { return bytesMeasured_; }

 private:
  std::array<draw::Face, kMostStyles> faces_{};
  draw::Face fallback_;
  std::uint64_t bytesMeasured_ = 0;
};

}  // namespace bn::layout
