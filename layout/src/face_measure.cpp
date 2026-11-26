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

}  // namespace

void FaceMeasure::SetFace(std::uint16_t style, draw::Face face) noexcept {
  if (style < kMostStyles) {
    faces_[style] = face;
  }
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

void FaceMeasure::Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
                          std::span<std::uint16_t> widths) noexcept {
  bytesMeasured_ += text.size();
  const std::size_t pieces = ends.size() < widths.size() ? ends.size() : widths.size();
  // A glyph's step is known once the next glyph is, which may be in a later
  // piece: so the last glyph seen is kept, with the piece it is in, and its
  // step goes to that piece's width when what follows it is known.
  draw::Glyph last;
  std::size_t lastPiece = pieces;  // None yet.
  bool apart = false;              // Something that is not drawn has come since the last glyph.
  std::uint32_t pixels = 0;        // Of the piece the last glyph is in, without the last glyph's step.
  std::size_t at = 0;
  for (std::size_t piece = 0; piece < pieces; ++piece) {
    widths[piece] = 0;
    const std::size_t end = ends[piece] < text.size() ? ends[piece] : text.size();
    while (at < end) {
      const core::Character character = core::CharacterAt(text, at);
      at += character.bytes;
      draw::Glyph found;
      if (!GlyphFor(style, character.good ? character.codePoint : kReplacement, found)) {
        apart = true;
        continue;
      }
      if (lastPiece != pieces) {
        pixels += apart ? PenStep(last) : PenStep(style, last, found);
        if (lastPiece != piece) {
          widths[lastPiece] = Width(pixels);
          pixels = 0;
        }
      }
      last = found;
      lastPiece = piece;
      apart = false;
    }
  }
  if (lastPiece != pieces) {
    widths[lastPiece] = Width(pixels + PenStep(last));
  }
}

std::uint16_t FaceMeasure::SpaceWidth(std::uint16_t style) noexcept {
  draw::Glyph glyph;
  return GlyphFor(style, kSpace, glyph) ? Width(PenStep(glyph)) : 0;
}

std::uint16_t FaceMeasure::HyphenWidth(std::uint16_t style) noexcept {
  draw::Glyph glyph;
  return HyphenGlyph(style, glyph) ? Width(PenStep(glyph)) : 0;
}

}  // namespace bn::layout
