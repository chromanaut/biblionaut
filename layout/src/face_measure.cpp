#include "layout/face_measure.h"

#include <algorithm>

#include "core/utf8.h"

namespace bn::layout {

namespace {

constexpr std::uint32_t kHyphen = 0x2010;
constexpr std::uint32_t kHyphenMinus = 0x002D;
constexpr std::uint32_t kSpace = 0x0020;
constexpr std::uint32_t kReplacement = 0xFFFD;  // What a byte that is not UTF-8 is drawn as.

// So many pixels as a width, which holds FaceMeasure::kMostPixels at the
// most.
std::uint16_t Width(std::uint32_t pixels) noexcept {
  return static_cast<std::uint16_t>(std::min<std::uint32_t>(pixels, FaceMeasure::kMostPixels) *
                                    FaceMeasure::kUnitsToAPixel);
}

// The bytes of `text` from `from` up to `end`, which are there.
std::string_view Part(std::string_view text, std::size_t from, std::size_t end) noexcept {
  return {text.data() + from, end - from};
}

}  // namespace

void FaceMeasure::SetFace(std::uint16_t style, draw::Face face) noexcept {
  if (style < kMostStyles) {
    faces_[style] = face;
  }
  for (Kept& kept : kept_) {
    kept.known = false;
  }
}

void FaceMeasure::SetFallback(draw::Face face) noexcept {
  fallback_ = face;
  for (Kept& kept : kept_) {
    kept.known = false;
  }
}

const FaceMeasure::Kept& FaceMeasure::KeptFor(std::uint16_t style) const noexcept {
  // A style with no face of its own has style 0's, as it has its letters.
  Kept& kept = kept_[style < kMostStyles && faces_[style].Valid() ? style : 0];
  if (!kept.known) {
    kept.hasHyphen = HyphenGlyph(style, kept.hyphen);
    kept.firsts = FaceOf(style).LigatureFirsts() | fallback_.LigatureFirsts();
    kept.known = true;
  }
  return kept;
}

draw::Face FaceMeasure::FaceOf(std::uint16_t style) const noexcept {
  if (style < kMostStyles && faces_[style].Valid()) {
    return faces_[style];
  }
  return faces_[0];
}

bool FaceMeasure::GlyphFor(std::uint16_t style, std::uint32_t codePoint, draw::Glyph& glyph) const noexcept {
  if (!Drawn(codePoint)) {
    return false;
  }
  const draw::Face face = FaceOf(style);
  return face.Find(codePoint, glyph) || fallback_.Find(codePoint, glyph) || face.Missing(glyph);
}

bool FaceMeasure::HyphenGlyph(std::uint16_t style, draw::Glyph& glyph) const noexcept {
  const draw::Face face = FaceOf(style);
  return face.Find(kHyphen, glyph) || face.Find(kHyphenMinus, glyph) || GlyphFor(style, kHyphenMinus, glyph);
}

std::size_t FaceMeasure::Join(std::uint16_t style, std::span<const draw::Glyph> glyphs,
                              draw::Glyph& ligature) const noexcept {
  // One face gave them all, or neither joins them: the style's own, or the
  // fallback, each of which joins nothing that is not its own.
  const std::size_t joined = FaceOf(style).Ligature(glyphs, ligature);
  return joined != 0 ? joined : fallback_.Ligature(glyphs, ligature);
}

std::uint16_t FaceMeasure::PenStep(std::uint16_t style, const draw::Glyph& glyph,
                                   const draw::Glyph& next) const noexcept {
  // One face gave both, or neither kerns them: the style's own, or the
  // fallback, each of which kerns nothing that is not its own.
  std::int8_t kerning = FaceOf(style).Kern(glyph, next);
  if (kerning == 0) {
    kerning = fallback_.Kern(glyph, next);
  }
  const std::int32_t quarters =
      std::max<std::int32_t>(std::int32_t{glyph.advance} + static_cast<std::int32_t>(kerning), 0);
  return static_cast<std::uint16_t>((quarters + (kUnitsToAPixel / 2)) / kUnitsToAPixel);
}

void FaceMeasure::Put(Pen& pen, std::uint16_t style, const draw::Glyph& glyph) const noexcept {
  pen.pixels = PenAfter(pen, style, &glyph);
  pen.last = glyph;
  pen.has = true;
}

void FaceMeasure::PutNothing(Pen& pen) noexcept {
  if (pen.has) {
    pen.pixels += PenStep(pen.last);
    pen.has = false;
  }
}

std::uint32_t FaceMeasure::PenAfter(const Pen& pen, std::uint16_t style, const draw::Glyph* next) const noexcept {
  if (!pen.has) {
    return pen.pixels;
  }
  return pen.pixels + (next != nullptr ? PenStep(style, pen.last, *next) : PenStep(pen.last));
}

std::uint32_t FaceMeasure::BeforeCut(Pen whole, GlyphRun rest, std::string_view text, std::size_t cut,
                                     std::uint16_t style) const noexcept {
  // The glyphs of the text from the cut on, and those of the whole after
  // the ligature, each gone on with while it is behind the other, until
  // both have a glyph begin at the same byte: from there on they are the
  // same glyphs.
  GlyphRun apart(*this, Part(text, cut, text.size()), style);
  Pen fresh;
  draw::Glyph glyph;
  bool drawn = false;
  const std::uint32_t beforeLigature = whole.pixels;
  for (std::size_t glyphs = 0;; ++glyphs) {
    const std::size_t freshAt = cut + apart.At();
    if (freshAt == rest.At()) {
      break;
    }
    if (glyphs == kMostGlyphsApart) {
      // Given up: the ligature is counted whole in the text after the cut,
      // which is then no narrower than it will be drawn.
      return beforeLigature;
    }
    const bool freshBehind = freshAt < rest.At();
    Pen& pen = freshBehind ? fresh : whole;
    if (!(freshBehind ? apart : rest).Next(glyph, drawn)) {
      break;
    }
    if (drawn) {
      Put(pen, style, glyph);
    } else {
      PutNothing(pen);
    }
  }
  // What follows both, if the text goes on.
  const bool more = rest.Next(glyph, drawn) && drawn;
  const draw::Glyph* next = more ? &glyph : nullptr;
  const std::uint32_t all = PenAfter(whole, style, next);
  const std::uint32_t after = PenAfter(fresh, style, next);
  return all > after ? all - after : 0;
}

void FaceMeasure::Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
                          std::span<std::uint16_t> widths, std::span<std::uint16_t> hyphens) noexcept {
  bytesMeasured_ += text.size();
  const std::size_t pieces = ends.size() < widths.size() ? ends.size() : widths.size();
  // Where piece `piece` ends, and the furthest any does: nothing past that
  // is looked at.
  const auto cutOf = [&](std::size_t piece) { return ends[piece] < text.size() ? ends[piece] : text.size(); };
  std::size_t furthest = 0;
  for (std::size_t piece = 0; piece < pieces; ++piece) {
    furthest = std::max<std::size_t>(furthest, cutOf(piece));
  }
  const std::string_view stretch = Part(text, 0, furthest);

  const Kept& kept = KeptFor(style);
  const draw::Glyph* hyphen = kept.hasHyphen ? &kept.hyphen : nullptr;
  const std::uint32_t hyphenStep = hyphen != nullptr ? PenStep(*hyphen) : 0;

  // The pieces given so far come to `given` pixels. A piece is given what
  // the text before its end comes to over that: `before`; and for its
  // hyphen what the text before its end comes to with a hyphen after it,
  // `hyphened`, over that again.
  std::size_t piece = 0;
  std::uint32_t given = 0;
  const auto give = [&](std::uint32_t before, std::uint32_t hyphened) {
    const std::uint32_t upTo = std::max(before, given);
    widths[piece] = Width(upTo - given);
    if (piece < hyphens.size()) {
      hyphens[piece] = Width(hyphened > upTo ? hyphened - upTo : 0);
    }
    given = upTo;
    ++piece;
  };

  GlyphRun run(*this, stretch, style);
  Pen pen;
  while (piece < pieces) {
    const std::size_t begin = run.At();
    draw::Glyph glyph;
    bool drawn = false;
    const bool more = run.Next(glyph, drawn);
    // Where the pen is with this glyph after the last one: found once, for
    // the pieces that end here and for going on.
    const std::uint32_t reached = PenAfter(pen, style, more && drawn ? &glyph : nullptr);
    // The pieces that end where this glyph begins: the last glyph before
    // them is kerned with this one where they are on a line together, and
    // with the hyphen where a line ends between them.
    while (piece < pieces && cutOf(piece) <= begin) {
      give(reached, PenAfter(pen, style, hyphen) + hyphenStep);
    }
    if (!more) {
      break;
    }
    // The pieces that end inside this glyph, which is then a ligature. With
    // a line's end there, its letters before the cut are joined by
    // themselves, and so are the letters after it. (A cut inside one
    // character leaves bytes on either side of it that are none, and they
    // are counted as they would be drawn.)
    while (piece < pieces && cutOf(piece) < run.At()) {
      const std::size_t cut = cutOf(piece);
      Pen head = pen;
      GlyphRun letters(*this, Part(stretch, begin, cut), style);
      draw::Glyph letter;
      bool letterDrawn = false;
      while (letters.Next(letter, letterDrawn)) {
        if (letterDrawn) {
          Put(head, style, letter);
        } else {
          PutNothing(head);
        }
      }
      Pen whole;
      whole.pixels = reached;
      whole.last = glyph;
      whole.has = drawn;
      give(BeforeCut(whole, run, stretch, cut, style), PenAfter(head, style, hyphen) + hyphenStep);
    }
    // Past this glyph, or past a character that is not drawn.
    pen.pixels = reached;
    pen.has = drawn;
    if (drawn) {
      pen.last = glyph;
    }
  }
}

std::uint16_t FaceMeasure::SpaceWidth(std::uint16_t style) noexcept {
  draw::Glyph glyph;
  return GlyphFor(style, kSpace, glyph) ? Width(PenStep(glyph)) : 0;
}

GlyphRun::GlyphRun(const FaceMeasure& measure, std::string_view text, std::uint16_t style) noexcept
    : measure_(&measure), text_(text), style_(style), marks_(measure.JoinMarks(style)) {}

void GlyphRun::LookAhead(std::size_t wanted) noexcept {
  while (waiting_ < wanted && read_ < text_.size()) {
    const core::Character character = core::CharacterAt(text_, read_);
    bytes_[waiting_] = static_cast<std::uint8_t>(character.bytes);
    drawn_[waiting_] =
        measure_->GlyphFor(style_, character.good ? character.codePoint : kReplacement, glyphs_[waiting_]);
    read_ += character.bytes;
    ++waiting_;
  }
}

bool GlyphRun::Next(draw::Glyph& glyph, bool& drawn) noexcept {
  LookAhead(1);
  if (waiting_ == 0) {
    return false;
  }
  drawn = drawn_[0];
  std::size_t taken = 1;
  if (drawn) {
    glyph = glyphs_[0];
    if (((marks_ >> (glyph.index & 63U)) & 1U) != 0) {
      // The characters that follow it with nothing between that is not
      // drawn: the face may draw two or three of them as one.
      LookAhead(kMostJoined);
      std::size_t together = 1;
      while (together < waiting_ && drawn_[together]) {
        ++together;
      }
      draw::Glyph ligature;
      const std::size_t joined =
          together > 1 ? measure_->Join(style_, std::span<const draw::Glyph>(glyphs_).first(together), ligature) : 0;
      if (joined > 1 && joined <= together) {
        glyph = ligature;
        taken = joined;
      }
    }
  }
  for (std::size_t i = 0; i < taken; ++i) {
    at_ += bytes_[i];
  }
  for (std::size_t i = taken; i < waiting_; ++i) {
    glyphs_[i - taken] = glyphs_[i];
    bytes_[i - taken] = bytes_[i];
    drawn_[i - taken] = drawn_[i];
  }
  waiting_ -= taken;
  return true;
}

}  // namespace bn::layout
