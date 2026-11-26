#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// Making fonts images (draw/font.h) in memory for tests, the plain way from
// the description of the layout: a face is its numbers and a list of glyphs,
// each with the code point it is for and its picture a level of ink to a
// pixel. Nothing here shares the font builder's code or the reader's.
//
// A face's kerning is given a pair of code points at a time. No two glyphs
// are put in one class here: each character that is the first of some pair
// has a class of its own as a first, in the order of the code points, and
// each that is the second of some pair likewise as a second.
//
// A face's ligatures are given by the characters they are made of. The
// glyph that stands for them is some character's own, or one of the face's
// extra glyphs, which no character has and which come after all the rest.

namespace bn::test {

struct ImageGlyph {
  std::uint16_t advance = 0;  // Quarters of a pixel.
  std::uint8_t width = 0;
  std::uint8_t height = 0;
  std::int8_t left = 0;
  std::int8_t top = 0;
  std::vector<std::uint8_t> ink;  // width * height levels, 0 to 3, row by row.
};

// A glyph `width` by `height` with every pixel `level`, the pen moved
// `advance` quarters.
inline ImageGlyph Block(std::uint16_t advance, std::uint8_t width, std::uint8_t height, std::uint8_t level = 3) {
  return {.advance = advance,
          .width = width,
          .height = height,
          .left = 0,
          .top = static_cast<std::int8_t>(height),
          .ink = std::vector<std::uint8_t>(std::size_t{width} * height, level)};
}

// A glyph with nothing to draw, as a space is: the pen moved `advance`
// quarters.
inline ImageGlyph Blank(std::uint16_t advance) {
  ImageGlyph glyph;
  glyph.advance = advance;
  return glyph;
}

// A ligature: the characters whose glyphs it is made of, two or three, and
// the glyph drawn in their place: the glyph of the character `glyph`, or,
// if `extra` is not less than nothing, that one of the face's extra glyphs.
struct ImageLigature {
  std::vector<std::uint32_t> letters;
  std::uint32_t glyph = 0;
  int extra = -1;
};

struct ImageFace {
  std::uint8_t role = 0;
  std::uint8_t style = 0;
  std::uint16_t size = 20;
  std::int16_t ascent = 16;
  std::int16_t descent = 5;
  std::uint16_t lineHeight = 24;
  std::uint16_t xHeight = 10;
  ImageGlyph missing = Block(40, 8, 12, 1);    // Glyph 0.
  std::map<std::uint32_t, ImageGlyph> glyphs;  // By code point.
  // How much further on the second of two characters is set after the
  // first, in quarters of a pixel. Both have to have glyphs.
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::int8_t> kerning;
  // Glyphs no character has, numbered after the characters' glyphs; and the
  // ligatures, in the order the face is to try them.
  std::vector<ImageGlyph> extras;
  std::vector<ImageLigature> ligatures;
};

// A face of `role` and `style` at `size`, with glyph 0 and no others.
inline ImageFace EmptyFace(std::uint8_t role, std::uint8_t style, std::uint16_t size = 20) {
  ImageFace face;
  face.role = role;
  face.style = style;
  face.size = size;
  return face;
}

// Where the parts of a face written by FontImageBytes() are, for tests that
// spoil them.
struct ImageFacePlaces {
  std::size_t record = 0;
  std::size_t ranges = 0;
  std::size_t rangeCount = 0;
  std::size_t glyphs = 0;
  std::size_t glyphCount = 0;
  std::size_t pictures = 0;
  std::size_t pictureBytes = 0;
  std::size_t kerning = 0;  // The table, a byte for each of `kernFirsts` by `kernSeconds`.
  std::size_t kernFirsts = 0;
  std::size_t kernSeconds = 0;
  std::size_t ligatures = 0;  // The list, eight bytes for each of `ligatureCount`.
  std::size_t ligatureCount = 0;
};

inline void PutLe16(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<std::uint8_t>(value);
  out[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}

inline void PutLe32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  PutLe16(out, at, value & 0xFFFFU);
  PutLe16(out, at + 2, value >> 16U);
}

// A picture's bytes: two bits a pixel, the first in the highest two bits.
inline std::vector<std::uint8_t> PackedInk(const std::vector<std::uint8_t>& ink) {
  std::vector<std::uint8_t> bytes(((ink.size() * 2) + 7) / 8, 0);
  for (std::size_t i = 0; i < ink.size(); ++i) {
    bytes[i / 4] = static_cast<std::uint8_t>(bytes[i / 4] | ((ink[i] & 3U) << (6U - ((i % 4) * 2))));
  }
  return bytes;
}

// The image of `faces`. `places`, if given, is filled with where each
// face's parts were put.
inline std::vector<std::uint8_t> FontImageBytes(const std::vector<ImageFace>& faces,
                                                std::vector<ImageFacePlaces>* places = nullptr) {
  constexpr std::size_t kHeader = 16;
  constexpr std::size_t kFace = 48;
  std::vector<std::uint8_t> out(kHeader + (faces.size() * kFace), 0);
  out[0] = 'B';
  out[1] = 'N';
  out[2] = 'F';
  out[3] = 'N';
  out[4] = 1;
  PutLe16(out, 6, static_cast<std::uint32_t>(faces.size()));
  if (places != nullptr) {
    places->clear();
  }
  for (std::size_t f = 0; f < faces.size(); ++f) {
    const ImageFace& face = faces[f];
    // The glyphs in order: 0, then by code point.
    std::vector<const ImageGlyph*> glyphs = {&face.missing};
    struct Range {
      std::uint32_t first;
      std::uint32_t count;
      std::uint32_t glyph;
    };
    std::vector<Range> ranges;
    std::map<std::uint32_t, std::uint32_t> numberOf;  // A character's glyph.
    for (const auto& [codePoint, glyph] : face.glyphs) {
      if (!ranges.empty() && ranges.back().first + ranges.back().count == codePoint) {
        ++ranges.back().count;
      } else {
        ranges.push_back({codePoint, 1, static_cast<std::uint32_t>(glyphs.size())});
      }
      numberOf[codePoint] = static_cast<std::uint32_t>(glyphs.size());
      glyphs.push_back(&glyph);
    }
    const std::size_t firstExtra = glyphs.size();
    for (const ImageGlyph& extra : face.extras) {
      glyphs.push_back(&extra);
    }
    ImageFacePlaces put;
    put.record = kHeader + (f * kFace);
    put.ranges = out.size();
    put.rangeCount = ranges.size();
    for (const Range& range : ranges) {
      const std::size_t at = out.size();
      out.resize(at + 12, 0);
      PutLe32(out, at, range.first);
      PutLe32(out, at + 4, range.count);
      PutLe32(out, at + 8, range.glyph);
    }
    // Kerning: a class for each character that is the first of a pair, and
    // one for each that is the second, from 1 in the order of the code
    // points.
    std::map<std::uint32_t, std::uint8_t> firstClass;
    std::map<std::uint32_t, std::uint8_t> secondClass;
    for (const auto& [pair, quarters] : face.kerning) {
      firstClass[pair.first] = 0;
      secondClass[pair.second] = 0;
    }
    std::uint8_t next = 0;
    for (auto& [codePoint, kernClass] : firstClass) {
      kernClass = ++next;
    }
    next = 0;
    for (auto& [codePoint, kernClass] : secondClass) {
      kernClass = ++next;
    }
    put.glyphs = out.size();
    put.glyphCount = glyphs.size();
    out.resize(put.glyphs + (glyphs.size() * 12), 0);
    std::size_t numbered = 1;
    for (const auto& [codePoint, glyph] : face.glyphs) {
      const std::size_t at = put.glyphs + (numbered * 12);
      out[at + 6] = firstClass.contains(codePoint) ? firstClass[codePoint] : 0;
      out[at + 7] = secondClass.contains(codePoint) ? secondClass[codePoint] : 0;
      ++numbered;
    }
    put.kerning = out.size();
    put.kernFirsts = firstClass.size();
    put.kernSeconds = secondClass.size();
    out.resize(put.kerning + (put.kernFirsts * put.kernSeconds), 0);
    for (const auto& [pair, quarters] : face.kerning) {
      out[put.kerning + ((firstClass[pair.first] - 1U) * put.kernSeconds) + (secondClass[pair.second] - 1U)] =
          static_cast<std::uint8_t>(quarters);
    }
    // The ligatures, after the kerning: the glyphs of the letters, 0 for a
    // third there is none of, and the glyph that stands for them.
    put.ligatures = out.size();
    put.ligatureCount = face.ligatures.size();
    for (const ImageLigature& ligature : face.ligatures) {
      const std::size_t at = out.size();
      out.resize(at + 8, 0);
      for (std::size_t letter = 0; letter < ligature.letters.size() && letter < 3; ++letter) {
        PutLe16(out, at + (2 * letter), numberOf[ligature.letters[letter]]);
      }
      PutLe16(out, at + 6,
              ligature.extra >= 0 ? static_cast<std::uint32_t>(firstExtra + static_cast<std::size_t>(ligature.extra))
                                  : numberOf[ligature.glyph]);
    }
    std::vector<std::uint8_t> pictures;
    for (std::size_t g = 0; g < glyphs.size(); ++g) {
      const ImageGlyph& glyph = *glyphs[g];
      const std::size_t at = put.glyphs + (g * 12);
      PutLe16(out, at, glyph.advance);
      out[at + 2] = glyph.width;
      out[at + 3] = glyph.height;
      out[at + 4] = static_cast<std::uint8_t>(glyph.left);
      out[at + 5] = static_cast<std::uint8_t>(glyph.top);
      PutLe32(out, at + 8, static_cast<std::uint32_t>(pictures.size()));
      const std::vector<std::uint8_t> packed = PackedInk(glyph.ink);
      pictures.insert(pictures.end(), packed.begin(), packed.end());
    }
    put.pictures = out.size();
    put.pictureBytes = pictures.size();
    out.insert(out.end(), pictures.begin(), pictures.end());

    out[put.record] = face.role;
    out[put.record + 1] = face.style;
    PutLe16(out, put.record + 2, face.size);
    PutLe16(out, put.record + 4, static_cast<std::uint16_t>(face.ascent));
    PutLe16(out, put.record + 6, static_cast<std::uint16_t>(face.descent));
    PutLe16(out, put.record + 8, face.lineHeight);
    PutLe16(out, put.record + 10, face.xHeight);
    PutLe32(out, put.record + 12, static_cast<std::uint32_t>(put.rangeCount));
    PutLe32(out, put.record + 16, static_cast<std::uint32_t>(put.ranges));
    PutLe32(out, put.record + 20, static_cast<std::uint32_t>(put.glyphCount));
    PutLe32(out, put.record + 24, static_cast<std::uint32_t>(put.glyphs));
    PutLe32(out, put.record + 28, static_cast<std::uint32_t>(put.pictures));
    PutLe32(out, put.record + 32, static_cast<std::uint32_t>(put.pictureBytes));
    if (put.kernFirsts * put.kernSeconds > 0) {
      PutLe32(out, put.record + 36, static_cast<std::uint32_t>(put.kerning));
      out[put.record + 40] = static_cast<std::uint8_t>(put.kernFirsts);
      out[put.record + 41] = static_cast<std::uint8_t>(put.kernSeconds);
    }
    if (put.ligatureCount > 0) {
      out[put.record + 42] = static_cast<std::uint8_t>(put.ligatureCount);
      PutLe32(out, put.record + 44, static_cast<std::uint32_t>(put.ligatures));
    }
    if (places != nullptr) {
      places->push_back(put);
    }
  }
  PutLe32(out, 8, static_cast<std::uint32_t>(out.size()));
  return out;
}

}  // namespace bn::test
