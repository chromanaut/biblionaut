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
// number of pixels after the one before it, whichever two they are: the
// first's advance and what their face kerns the two by, both of which an
// image has in quarters of a pixel, added and then put to the nearest pixel
// (PenStep()). So the same two letters are the same distance apart wherever
// they come on a page. Adding the advances up as they are and rounding
// where the pen has got to would not give that: two letters would be a
// pixel further apart in one place than in another, by what was left over
// before them, and that is plain to the eye. Nor would rounding the kerning
// by itself: half a pixel of it would be a whole one or none.
//
// Two glyphs are kerned when one follows the other in a stretch of text
// that is measured as one, which is as much of a word as is in one style,
// and one face gave both. So nothing is kerned across a space, across a
// change of style, or between a letter and a glyph the fallback face lent
// for a character its neighbour's face lacks. Nor across a soft hyphen or a
// zero width space, which are not drawn: the setter measures what is before
// one apart from what is after. Where the hyphenator would let a word be
// broken makes no difference to it: the last letter before such a place is
// kerned with the first after it, and is as wide as that whether or not a
// line ends there.
//
// Widths are in quarters of a pixel, as the setter counts, and each is a
// whole number of pixels: a piece of a word is as wide as its characters'
// steps added up, and a space and a hyphen are rounded as a letter is. So
// what is drawn from the pieces' widths lines up exactly with what was
// measured, by whoever moves the pen by the same steps: after each glyph
// but the last of a stretch, the step it has before the next (the three
// argument PenStep()); and after the last, its own.
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
  // How many pixels the pen moves on after `glyph` when `next` is drawn
  // straight after it, both in `style`: the glyph's advance and what their
  // face kerns the two by, added in quarters of a pixel and then put to the
  // nearest pixel, a half going up, and never less than nothing. Glyphs of
  // two faces are not kerned, and the step is then the first's own.
  std::uint16_t PenStep(std::uint16_t style, const draw::Glyph& glyph, const draw::Glyph& next) const noexcept;

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
