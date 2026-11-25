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
