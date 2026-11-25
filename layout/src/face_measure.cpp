#include "layout/face_measure.h"

#include "core/utf8.h"

namespace bn::layout {

namespace {

constexpr std::uint32_t kHyphen = 0x2010;
constexpr std::uint32_t kHyphenMinus = 0x002D;
constexpr std::uint32_t kSpace = 0x0020;
constexpr std::uint32_t kReplacement = 0xFFFD;  // What a byte that is not UTF-8 is drawn as.

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

void FaceMeasure::Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
                          std::span<std::uint16_t> widths) noexcept {
  bytesMeasured_ += text.size();
  std::size_t at = 0;
  for (std::size_t piece = 0; piece < ends.size() && piece < widths.size(); ++piece) {
    std::uint32_t width = 0;
    const std::size_t end = ends[piece] < text.size() ? ends[piece] : text.size();
    while (at < end) {
      const core::Character character = core::CharacterAt(text, at);
      at += character.bytes;
      draw::Glyph glyph;
      if (GlyphFor(style, character.good ? character.codePoint : kReplacement, glyph)) {
        width += glyph.advance;
      }
    }
    widths[piece] = static_cast<std::uint16_t>(width < 0xFFFFU ? width : 0xFFFFU);
  }
}

std::uint16_t FaceMeasure::SpaceWidth(std::uint16_t style) noexcept {
  draw::Glyph glyph;
  return GlyphFor(style, kSpace, glyph) ? glyph.advance : 0;
}

std::uint16_t FaceMeasure::HyphenWidth(std::uint16_t style) noexcept {
  draw::Glyph glyph;
  return HyphenGlyph(style, glyph) ? glyph.advance : 0;
}

}  // namespace bn::layout
