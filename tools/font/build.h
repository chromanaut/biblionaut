#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "font/preset.h"

// Makes the fonts image that draw/font.h reads, from a preset's font files,
// drawing each glyph with FreeType.
//
// Each face of the preset is drawn at each size. A glyph's picture is
// FreeType's, with light hinting (which moves outlines up and down to the
// pixels, and not across), its 256 grays made four levels of ink. Its
// advance is the outline's, in quarters of a pixel, so that the widths of a
// line's characters add up to what the face says the line is wide, at any
// size. Glyph 0 of each face is the font's own glyph for what it lacks.
//
// A face is kerned as its font is (font/kerning.h says what is read of a
// font's kerning, and what is left out): each pair to the nearest quarter of
// a pixel at the face's size, so a pair kerned by less than an eighth of a
// pixel there is not kerned in that face. A face whose font has no kerning,
// as a monospace font has none, has none.

namespace bn::font_tool {

// What went into one face of the image.
struct BuiltFace {
  draw::FaceRole role = draw::FaceRole::kSerif;
  std::uint8_t style = 0;
  std::uint16_t size = 0;
  std::size_t glyphs = 0;   // Glyph 0 among them.
  std::size_t lacking = 0;  // Characters asked for that the font has no glyph for.
  std::size_t pictureBytes = 0;
  std::int16_t ascent = 0;
  std::int16_t descent = 0;
  std::uint16_t lineHeight = 0;
  std::uint16_t xHeight = 0;
  // Its kerning: whether it has any in the image, how many pairs of its
  // glyphs are kerned, and how many classes its glyphs are in as the first
  // of two and as the second. A face with more classes than an image holds
  // has the last three and is not kerned.
  bool kerned = false;
  std::size_t kernedPairs = 0;
  std::size_t kernFirsts = 0;
  std::size_t kernSeconds = 0;
  // What its font has of kerning that was left out: lookups that look at
  // more than two glyphs, and pairs that adjust more than an advance.
  std::size_t kernLookupsLeftOut = 0;
  std::size_t kernAdjustmentsLeftOut = 0;
};

struct Built {
  bool ok = false;
  std::string error;  // If not: what went wrong, and with which file.
  std::vector<std::uint8_t> image;
  std::vector<BuiltFace> faces;
  // What was done instead of what the preset asked: a style it does not
  // name, for one.
  std::vector<std::string> warnings;
};

Built BuildImage(const Preset& preset);

}  // namespace bn::font_tool
