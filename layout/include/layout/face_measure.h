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
// what it lacks.
//
// Some characters are never drawn and take no room, whatever glyph a face
// has for them: those Unicode says are shown as nothing where there is no
// glyph for them (its property Default_Ignorable_Code_Point). A soft hyphen
// and a zero width space are two. So are the word joiner and U+FEFF that a
// book puts where no line is to end, the selector after a symbol that says
// which form of it is meant, and the marks that say which way text runs.
//
// Letters a face joins are drawn as one glyph, a ligature: an f and an i as
// the face's fi. A stretch of text is gone through from its start, and at
// each place the first of the face's ligatures that is made of the two or
// three glyphs there is drawn in their place (GlyphRun). A ligature is of
// one face's glyphs, so nothing the fallback face lent is joined with a
// letter of the style's own.
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
// Two glyphs are kerned, and letters are joined, when one follows the other
// in a stretch of text that is measured as one, which is as much of a word
// as is in one style. So nothing is kerned or joined across a space or
// across a change of style. Nor across a character that is not drawn: the
// glyph before one is followed by the next as the last glyph of a stretch
// is, by its own advance. (The setter measures what is before a soft hyphen
// or a zero width space apart from what is after. The others come to the
// measure inside their word.)
//
// A line's text is drawn as it would be by itself. Where a line ends inside
// a word, at a place the hyphenator found, the letters before the place are
// joined with one another and not with those after it, and the last of them
// is kerned with the hyphen and not with the letter that begins the next
// line; and the next line's letters are joined from its first. So "office"
// is an o and one glyph for its two f's and its i where it is whole, and an
// o, an f and a hyphen, and then the glyph for fi, a c and an e, where a
// line ends between its f's. The widths Measure() gives count each of these
// as it is drawn: the pieces from a cut on come to the text from the cut on
// set by itself, and a piece's number for its hyphen is what a line that
// has the stretch from its start and ends there with a hyphen is wider by
// than its pieces. A line that has a stretch's middle only, begun at one
// cut and ended at another, is counted exactly as well wherever it has a
// glyph that neither cut changes: one that is the same glyph of the same
// letters in the whole stretch, in the text from the first cut on, and in
// the text up to the second.
//
// Where a cut falls between the letters of a ligature, the glyphs after it
// are found afresh from the cut until they fall in with those of the whole
// stretch, which with ligatures of f, i and l is at once or a glyph later.
// That is given up after kMostGlyphsApart glyphs, so that no text makes a
// call long: in a word of nothing but f's, the text after such a cut is
// then counted with the ligature whole, which is wider than it is drawn
// and never narrower.
//
// Widths are in quarters of a pixel, as the setter counts, and each is a
// whole number of pixels: a space is rounded as a letter is. So what is
// drawn lines up exactly with what was measured, by whoever goes through
// each stretch of a line with a GlyphRun and moves the pen by the same
// steps: after each glyph but the last of a stretch, the step it has before
// the next (the three argument PenStep()); after the last, its own; and
// where a hyphen is shown, the last glyph's step before the hyphen's glyph.
//
// The hyphen shown where a line ends inside a word is the style's U+2010,
// or its hyphen-minus if it has none.
//
// A style's hyphen, and which of its glyphs begin a ligature, are found
// the first time text in the style is measured or gone through, and kept
// until a face is set. So faces are set once they are held, and set again
// when they have been given up and are held anew.

namespace bn::layout {

class GlyphRun;

class FaceMeasure final : public TextMeasure {
 public:
  // The most styles: as many as one word may be in.
  static constexpr std::size_t kMostStyles = ParagraphSetter::kMaxStyles;
  // The most glyphs gone through after a cut inside a ligature, of the text
  // from the cut on and of the whole stretch together, to find where the two
  // fall in with one another.
  static constexpr std::size_t kMostGlyphsApart = 8;

  // The face text in `style` is drawn in. A style given none takes style
  // 0's.
  void SetFace(std::uint16_t style, draw::Face face) noexcept;
  void SetFallback(draw::Face face) noexcept;
  draw::Face FaceOf(std::uint16_t style) const noexcept;

  // The glyph `codePoint` is drawn with in `style` when it is joined with
  // nothing, into `glyph`. False for a character that is not drawn, or when
  // there is no glyph to draw at all.
  bool GlyphFor(std::uint16_t style, std::uint32_t codePoint, draw::Glyph& glyph) const noexcept;
  // The glyph of the hyphen shown at a line's end.
  bool HyphenGlyph(std::uint16_t style, draw::Glyph& glyph) const noexcept;
  // The glyph drawn in place of the first two or three of `glyphs`, which
  // follow one another in `style`, into `ligature`, and how many it is drawn
  // in place of: 0 if they are not joined.
  std::size_t Join(std::uint16_t style, std::span<const draw::Glyph> glyphs, draw::Glyph& ligature) const noexcept;
  // A mark for the glyphs that may begin a ligature in `style`, as
  // draw::Face::LigatureFirsts() gives them, of the style's face and the
  // fallback's together: nothing is joined that begins with a glyph whose
  // mark is not set.
  std::uint64_t JoinMarks(std::uint16_t style) const noexcept { return KeptFor(style).firsts; }
  // Whether anything in `style` is ever joined.
  bool Joins(std::uint16_t style) const noexcept { return JoinMarks(style) != 0; }

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
               std::span<std::uint16_t> widths, std::span<std::uint16_t> hyphens) noexcept override;
  std::uint16_t SpaceWidth(std::uint16_t style) noexcept override;

  // Whether `codePoint` is drawn at all: every character is but those with
  // Unicode's property Default_Ignorable_Code_Point.
  static bool Drawn(std::uint32_t codePoint) noexcept;

  // How many bytes it has been given to measure, over all its calls.
  std::uint64_t BytesMeasured() const noexcept { return bytesMeasured_; }

 private:
  // Where a pen has got to along some glyphs: the pixels before the last of
  // them, whose step is not known until what follows it is.
  struct Pen {
    std::uint32_t pixels = 0;
    draw::Glyph last;
    bool has = false;  // There is a last glyph, with nothing yet after it.
  };

  // Moves the pen past its last glyph, which `glyph` follows.
  void Put(Pen& pen, std::uint16_t style, const draw::Glyph& glyph) const noexcept;
  // Moves the pen past its last glyph, which nothing drawn follows.
  static void PutNothing(Pen& pen) noexcept;
  // Where the pen is after its last glyph if `next` follows that: null for
  // nothing.
  std::uint32_t PenAfter(const Pen& pen, std::uint16_t style, const draw::Glyph* next) const noexcept;
  // How wide the pieces of `text` before byte `cut` come to, where the cut
  // is between the letters of a ligature: what the whole is wider by than
  // the text from the cut on. `whole` is the pen through the glyphs of the
  // whole up to that ligature, and `rest` goes through those after it.
  std::uint32_t BeforeCut(Pen whole, GlyphRun rest, std::string_view text, std::size_t cut,
                          std::uint16_t style) const noexcept;

  // What is found for a style the first time it is wanted and kept until a
  // face is set: the glyph of its hyphen, if it has one, and the marks of
  // the glyphs that may begin a ligature.
  struct Kept {
    bool known = false;
    bool hasHyphen = false;
    draw::Glyph hyphen;
    std::uint64_t firsts = 0;
  };
  const Kept& KeptFor(std::uint16_t style) const noexcept;

  std::array<draw::Face, kMostStyles> faces_{};
  draw::Face fallback_;
  mutable std::array<Kept, kMostStyles> kept_{};
  std::uint64_t bytesMeasured_ = 0;
};

// Goes through a stretch of text in one style a glyph at a time, as the
// text is measured and is to be drawn: a ligature where the face joins the
// two or three characters there, and a character's own glyph otherwise.
//
// The measure and the text have to stay as they are while it is used.
class GlyphRun {
 public:
  GlyphRun(const FaceMeasure& measure, std::string_view text, std::uint16_t style) noexcept;

  // How many bytes of the text the glyphs given so far stand for: where the
  // next begins.
  std::size_t At() const noexcept { return at_; }
  // The next glyph, into `glyph`. `drawn` is false for a character that is
  // not drawn, which has no glyph, and nothing is joined or kerned across
  // one. Returns false at the end of the text.
  bool Next(draw::Glyph& glyph, bool& drawn) noexcept;

 private:
  // The most characters a face joins.
  static constexpr std::size_t kMostJoined = 3;

  // Looks up characters until `wanted` are waiting, or the text ends.
  void LookAhead(std::size_t wanted) noexcept;

  const FaceMeasure* measure_;
  std::string_view text_;
  std::uint16_t style_;
  std::uint64_t marks_;   // Of the glyphs that may begin a ligature.
  std::size_t at_ = 0;    // Where the first character waiting begins.
  std::size_t read_ = 0;  // Where the first not yet looked up begins.
  // The characters looked up and not yet given out: each one's glyph, how
  // many bytes it has, and whether it is drawn.
  std::array<draw::Glyph, kMostJoined> glyphs_{};
  std::array<std::uint8_t, kMostJoined> bytes_{};
  std::array<bool, kMostJoined> drawn_{};
  std::size_t waiting_ = 0;
};

}  // namespace bn::layout
