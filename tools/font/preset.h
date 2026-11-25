#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/font.h"

// A preset: which font file each face of the fonts image is drawn from, and
// at which sizes. It is a text file, a line to each thing it says, anything
// from a `#` to the end of a line passed over:
//
//   sizes 26 30 34
//   serif regular      Fonts/Serif-Regular.ttf
//   serif bold-italic  /Library/Fonts/Serif-BoldItalic.ttf
//   mono bold          Mono-Bold.ttf
//   characters U+0020-U+007E U+00A0
//
// A face's line is its role (serif, sans, mono or fallback), its style
// (regular, italic, bold or bold-italic) and the file, which is the rest of
// the line and may have spaces in it; a file named without a leading `/` is
// found from the folder the preset is in. `sizes` are the steps of the size
// ladder, in pixels to the em: every face is drawn at each. `characters`,
// which may be given more than once, are the code points that are drawn,
// one or a range, written U+ and hexadecimal; without them, the set the
// firmware is built to show (DefaultCharacters()).
//
// `bn-font build` takes several presets and reads them as one
// (ReadPresets()), and with `--fonts FOLDER` finds the files of all of them
// from that folder and not from each preset's own.

namespace bn::font_tool {

struct FaceFile {
  draw::FaceRole role = draw::FaceRole::kSerif;
  std::uint8_t style = 0;  // draw::kBold, draw::kItalic.
  std::string path;
  std::size_t line = 0;  // Where in the preset it was named.
};

// Code points from the first to the last, both in.
using CodePoints = std::pair<std::uint32_t, std::uint32_t>;

struct Preset {
  bool ok = false;
  std::string error;  // If not: what is wrong, with the line it is on.
  std::vector<std::uint16_t> sizes;
  std::vector<FaceFile> faces;
  std::vector<CodePoints> characters;
};

// Reads code points as a preset writes them: U+0041 for one, or
// U+0041-U+005A for those from the first to the last. False if `text` is
// neither.
bool ReadCodePoints(std::string_view text, CodePoints& codePoints);

// Reads a preset. `folder` is where files not named from `/` are found.
Preset ReadPreset(std::string_view text, std::string_view folder);

// The text of one preset file, what to call it when something in it is
// wrong, and where its files not named from `/` are found.
struct PresetText {
  std::string name;
  std::string text;
  std::string folder;
};

// Reads several presets as one: the sizes of all of them, the faces of all
// of them, and the characters of all of them. So a preset can be kept in
// parts (the sizes, a family's faces, a monospace face) that are put
// together when an image is built. A face named in two of them is wrong, as
// it is in one.
Preset ReadPresets(std::span<const PresetText> presets);

// The characters drawn when a preset names none: Latin with its
// extensions, the phonetic alphabet, the marks set over letters, Greek
// (polytonic too) and Cyrillic, the punctuation, currency, letterlike
// symbols, number forms, arrows and mathematical operators, and the
// ligatures of the alphabetic presentation forms.
const std::vector<CodePoints>& DefaultCharacters();

// The names used for a role and a style in a preset, and in what bn-font
// says.
std::string_view RoleName(draw::FaceRole role);
std::string_view StyleName(std::uint8_t style);

}  // namespace bn::font_tool
