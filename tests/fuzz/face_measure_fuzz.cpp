// The fuzz target for the measure of text in a fonts image's faces
// (layout/face_measure.h), which is given the text of a book a stretch at a
// time.
//
// What it is given:
//
//   byte 0   bits 0 and 1: the style the text is in: the serif, its italic,
//                          one that has no face of its own, or one past the
//                          last there can be
//            bit 2:        there is no fallback face
//
//   then the text. A byte of 0 in it is not text: the stretch is cut into
//   pieces there. Text past 255 bytes is left out, and so are cuts past the
//   most pieces the paragraph setter asks for at once.
//
// The faces are the same every time, made here: a serif that kerns and
// joins letters, among them two pairs that are each other's letters the
// other way round, so that the glyphs after a cut need never fall in with
// those of the whole; an italic that joins less; and a fallback face with
// letters neither has. The serif and the fallback each have a glyph for a
// character that is never drawn.
//
// What has to hold, whatever the bytes are:
//
//   - going through the text a glyph at a time gives the glyphs a plain
//     second setting of it gives, written here from the faces as they were
//     described and from what the header says: each standing for the same
//     bytes, every byte stood for once, nothing joined across what is not
//     drawn or across two faces, and no glyph for a character that is never
//     drawn, though a face has one for it;
//   - each piece is as wide, and each piece's hyphen comes to as much, as
//     the plain setting makes them: the text from a cut on set by itself,
//     the text up to a cut with a hyphen after it, and the glyphs after a
//     cut inside a ligature followed for as many glyphs as the header says
//     and no further;
//   - every width is a whole number of pixels, and the pieces of a stretch
//     that is cut nowhere inside a ligature add up to the whole.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/utf8.h"
#include "draw/font.h"
#include "font_image.h"
#include "fuzz_support.h"
#include "layout/face_measure.h"

namespace {

using bn::draw::FontImage;
using bn::layout::FaceMeasure;
using bn::layout::GlyphRun;
using bn::test::Block;
using bn::test::ImageFace;

constexpr std::size_t kMostText = 255;
constexpr std::size_t kMostPieces = 64;
constexpr std::uint32_t kHyphen = 0x2010;
constexpr std::uint32_t kHyphenMinus = '-';
constexpr std::uint32_t kReplacement = 0xFFFD;

// The faces: the serif, its italic and the fallback.
std::vector<ImageFace> Faces() {
  ImageFace serif = bn::test::EmptyFace(0, 0);
  ImageFace italic = bn::test::EmptyFace(0, bn::draw::kItalic);
  for (std::uint32_t letter = 'a'; letter <= 'z'; ++letter) {
    serif.glyphs[letter] = Block(static_cast<std::uint16_t>(40 + letter - 'a'), 8, 10);
    italic.glyphs[letter] = Block(static_cast<std::uint16_t>(50 + letter - 'a'), 8, 10);
  }
  serif.glyphs['-'] = Block(24, 5, 2);
  serif.glyphs[kHyphen] = Block(26, 5, 2);
  serif.glyphs[kReplacement] = Block(60, 10, 12);
  serif.glyphs[0xFB01] = Block(80, 16, 10);
  serif.glyphs[0xFB02] = Block(84, 16, 10);
  serif.missing = Block(36, 8, 12);
  // A glyph for the word joiner, as some fonts have, and one in the
  // fallback for a selector: neither is ever drawn.
  serif.glyphs[0x2060] = Block(33, 4, 4);
  // Two f's and an i, two f's, an a and a b (narrower than a b), a b and
  // an a (wider than a b, a hyphen and an a), three a's.
  serif.extras = {Block(110, 24, 10), Block(78, 16, 10), Block(30, 6, 10), Block(160, 20, 10), Block(50, 10, 10)};
  serif.ligatures = {{.letters = {'f', 'f', 'i'}, .extra = 0}, {.letters = {'f', 'i'}, .glyph = 0xFB01},
                     {.letters = {'f', 'f'}, .extra = 1},      {.letters = {'f', 'l'}, .glyph = 0xFB02},
                     {.letters = {'a', 'a', 'a'}, .extra = 4}, {.letters = {'a', 'b'}, .extra = 2},
                     {.letters = {'b', 'a'}, .extra = 3}};
  serif.kerning[{'q', 'u'}] = -2;
  serif.kerning[{'k', 'z'}] = -1;
  serif.kerning[{'z', 'k'}] = 6;
  serif.kerning[{'k', 'k'}] = -60;
  serif.kerning[{0xFB01, 'c'}] = -4;
  serif.kerning[{'c', 0xFB01}] = 3;
  serif.kerning[{'f', kHyphen}] = -6;
  serif.kerning[{'b', kHyphen}] = 5;
  serif.kerning[{'a', 'a'}] = -3;
  italic.glyphs['-'] = Block(28, 5, 2);
  italic.missing = Block(38, 8, 12);
  italic.extras = {Block(90, 16, 10)};
  italic.ligatures = {{.letters = {'f', 'i'}, .extra = 0}};
  italic.kerning[{'k', 'z'}] = -9;
  italic.kerning[{'f', '-'}] = -7;
  ImageFace fallback = bn::test::EmptyFace(3, 0);
  fallback.glyphs['a'] = Block(99, 8, 10);  // Never used: the others have one.
  fallback.glyphs[0x3B1] = Block(45, 8, 10);
  fallback.glyphs[0x3B3] = Block(48, 8, 10);
  fallback.glyphs[0x3B4] = Block(44, 8, 10);
  fallback.glyphs[0x3B5] = Block(44, 8, 10);
  fallback.glyphs[0xFE0E] = Block(37, 4, 4);
  fallback.extras = {Block(70, 14, 10)};
  fallback.ligatures = {{.letters = {0x3B4, 0x3B5}, .extra = 0}};
  fallback.kerning[{0x3B1, 0x3B3}] = -8;
  return {serif, italic, fallback};
}

// Whether a character is one of those that are never drawn: the ones with
// Unicode's property Default_Ignorable_Code_Point, written out here a second
// time, those that stand alone and then the runs.
bool NeverDrawn(std::uint32_t codePoint) {
  switch (codePoint) {
    case 0x00AD:
    case 0x034F:
    case 0x061C:
    case 0x115F:
    case 0x1160:
    case 0x17B4:
    case 0x17B5:
    case 0x3164:
    case 0xFEFF:
    case 0xFFA0:
      return true;
    default:
      break;
  }
  const auto within = [codePoint](std::uint32_t first, std::uint32_t count) {
    return codePoint >= first && codePoint - first < count;
  };
  return within(0x180B, 5) || within(0x200B, 5) || within(0x202A, 5) || within(0x2060, 16) || within(0xFE00, 16) ||
         within(0xFFF0, 9) || within(0x1BCA0, 4) || within(0x1D173, 8) || within(0xE0000, 4096);
}

// One glyph of the plain setting: which face's, and which of its glyphs: a
// character's, one of its extra glyphs, or its glyph for what it lacks.
struct Plain {
  const ImageFace* face = nullptr;  // Null for a character that is not drawn.
  std::uint32_t code = 0;           // The character whose glyph it is, if it is one's.
  int extra = -1;                   // Which extra glyph, if it is one.
  bool lacking = false;             // The face's glyph for what it lacks.
  std::size_t begin = 0;            // The bytes of the text it stands for.
  std::size_t end = 0;

  bool Drawn() const { return face != nullptr; }
  bool OfACharacter() const { return Drawn() && extra < 0 && !lacking; }
  std::uint32_t Advance() const {
    if (lacking) {
      return face->missing.advance;
    }
    return extra >= 0 ? face->extras[static_cast<std::size_t>(extra)].advance : face->glyphs.at(code).advance;
  }
};

// The plain setting: the faces as they were described, and which of them a
// style's text is in.
struct Setting {
  const ImageFace* own = nullptr;
  const ImageFace* fallback = nullptr;

  // A character's own glyph: its style's face's, the fallback's, or the
  // style's face's glyph for what it lacks.
  Plain GlyphOf(std::uint32_t codePoint) const {
    Plain glyph;
    if (NeverDrawn(codePoint)) {
      return glyph;
    }
    glyph.code = codePoint;
    if (own->glyphs.contains(codePoint)) {
      glyph.face = own;
    } else if (fallback != nullptr && fallback->glyphs.contains(codePoint)) {
      glyph.face = fallback;
    } else {
      glyph.face = own;
      glyph.lacking = true;
    }
    return glyph;
  }

  Plain HyphenGlyph() const { return own->glyphs.contains(kHyphen) ? GlyphOf(kHyphen) : GlyphOf(kHyphenMinus); }

  // The glyphs of `text` from byte `from` up to `to`, set by itself: at
  // each place the first of the face's ligatures whose letters are the
  // characters there, with nothing between them that is not drawn, and
  // otherwise the character's own.
  std::vector<Plain> Glyphs(std::string_view text, std::size_t from, std::size_t to) const {
    std::vector<Plain> singles;
    for (std::size_t at = from; at < to;) {
      const bn::core::Character character = bn::core::CharacterAt(text.substr(0, to), at);
      Plain glyph = GlyphOf(character.good ? character.codePoint : kReplacement);
      glyph.begin = at;
      glyph.end = at + character.bytes;
      singles.push_back(glyph);
      at = glyph.end;
    }
    std::vector<Plain> glyphs;
    for (std::size_t i = 0; i < singles.size();) {
      Plain glyph = singles[i];
      std::size_t taken = 1;
      if (glyph.Drawn()) {
        for (const bn::test::ImageLigature& ligature : glyph.face->ligatures) {
          bool fits = i + ligature.letters.size() <= singles.size();
          for (std::size_t letter = 0; fits && letter < ligature.letters.size(); ++letter) {
            const Plain& one = singles[i + letter];
            fits = one.OfACharacter() && one.face == glyph.face && one.code == ligature.letters[letter];
          }
          if (fits) {
            taken = ligature.letters.size();
            glyph.end = singles[i + taken - 1].end;
            glyph.extra = ligature.extra;
            glyph.code = ligature.extra < 0 ? ligature.glyph : 0;
            break;
          }
        }
      }
      glyphs.push_back(glyph);
      i += taken;
    }
    return glyphs;
  }

  // How many pixels the pen moves on after `glyph` with `next` after it,
  // which is null for nothing.
  static std::uint32_t Step(const Plain& glyph, const Plain* next) {
    int quarters = static_cast<int>(glyph.Advance());
    if (next != nullptr && next->Drawn() && next->face == glyph.face && glyph.OfACharacter() && next->OfACharacter()) {
      const auto pair = glyph.face->kerning.find({glyph.code, next->code});
      quarters += pair == glyph.face->kerning.end() ? 0 : pair->second;
    }
    return static_cast<std::uint32_t>(std::max(quarters, 0) + 2) / 4U;
  }

  // How wide `glyphs` are with `next` after the last of them: each moved on
  // from by its step before the one after it, or by its own before what is
  // not drawn.
  static std::uint32_t Width(const std::vector<Plain>& glyphs, const Plain* next) {
    std::uint32_t pixels = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      if (glyphs[i].Drawn()) {
        pixels += Step(glyphs[i], i + 1 < glyphs.size() ? &glyphs[i + 1] : next);
      }
    }
    return pixels;
  }
};

// What the measure gave a stretch's pieces, in pixels.
struct Measured {
  std::vector<std::uint32_t> widths;
  std::vector<std::uint32_t> hyphens;
};

Measured MeasureOf(FaceMeasure& measure, std::string_view text, std::uint16_t style,
                   const std::vector<std::uint8_t>& ends) {
  // Room past the pieces, to see that nothing is written there.
  std::vector<std::uint16_t> widths(ends.size() + 1, 0xFFFF);
  std::vector<std::uint16_t> hyphens(ends.size() + 1, 0xFFFF);
  const std::uint64_t before = measure.BytesMeasured();
  measure.Measure(text, style, ends, std::span<std::uint16_t>(widths).first(ends.size()),
                  std::span<std::uint16_t>(hyphens).first(ends.size()));
  BN_FUZZ_CHECK(measure.BytesMeasured() == before + text.size(), "the bytes measured were not counted");
  BN_FUZZ_CHECK(widths.back() == 0xFFFF && hyphens.back() == 0xFFFF, "a width was written past the pieces");
  Measured measured;
  measured.widths.reserve(ends.size());
  measured.hyphens.reserve(ends.size());
  for (std::size_t i = 0; i < ends.size(); ++i) {
    BN_FUZZ_CHECK(widths[i] % 4 == 0 && hyphens[i] % 4 == 0, "a width is not a whole number of pixels");
    measured.widths.push_back(widths[i] / 4U);
    measured.hyphens.push_back(hyphens[i] / 4U);
  }
  return measured;
}

// What the plain setting makes of the same pieces.
Measured PlainMeasureOf(const Setting& setting, std::string_view text, const std::vector<std::uint8_t>& ends) {
  const std::vector<Plain> whole = setting.Glyphs(text, 0, text.size());
  const Plain hyphen = setting.HyphenGlyph();
  const std::uint32_t hyphenStep = Setting::Step(hyphen, nullptr);
  Measured measured;
  measured.widths.reserve(ends.size());
  measured.hyphens.reserve(ends.size());
  std::uint32_t given = 0;
  for (const std::uint8_t end : ends) {
    const std::size_t cut = end;
    // The glyphs of the whole before the cut, and the one the cut is
    // inside, if it is inside one.
    std::size_t before = 0;
    while (before < whole.size() && whole[before].end <= cut) {
      ++before;
    }
    const bool inside = before < whole.size() && whole[before].begin < cut;
    const std::vector<Plain> first(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(before));
    std::uint32_t upTo = 0;
    std::uint32_t hyphened = 0;
    if (!inside) {
      upTo = Setting::Width(first, before < whole.size() ? &whole[before] : nullptr);
      hyphened = Setting::Width(first, &hyphen) + hyphenStep;
    } else {
      // The letters of the ligature before the cut, joined by themselves,
      // and the hyphen after them.
      std::vector<Plain> head = first;
      const std::vector<Plain> letters = setting.Glyphs(text, whole[before].begin, cut);
      head.insert(head.end(), letters.begin(), letters.end());
      hyphened = Setting::Width(head, &hyphen) + hyphenStep;
      // The glyphs from the cut on, and those of the whole after the
      // ligature: whichever is behind is gone on with, until both have a
      // glyph begin at the same byte, or so many have been gone through.
      const std::vector<Plain> fresh = setting.Glyphs(text, cut, text.size());
      std::size_t freshCount = 0;
      std::size_t wholeCount = before + 1;
      std::size_t freshAt = cut;
      std::size_t wholeAt = whole[before].end;
      bool fellIn = true;
      for (std::size_t glyphs = 0; freshAt != wholeAt; ++glyphs) {
        if (glyphs == FaceMeasure::kMostGlyphsApart) {
          fellIn = false;
          break;
        }
        if (freshAt < wholeAt) {
          freshAt = fresh[freshCount++].end;
        } else {
          wholeAt = whole[wholeCount++].end;
        }
      }
      if (fellIn) {
        const Plain* next = wholeCount < whole.size() ? &whole[wholeCount] : nullptr;
        const std::uint32_t all = Setting::Width(
            std::vector<Plain>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(wholeCount)), next);
        const std::uint32_t after = Setting::Width(
            std::vector<Plain>(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(freshCount)), next);
        upTo = all > after ? all - after : 0;
      } else {
        upTo = Setting::Width(first, &whole[before]);
      }
    }
    upTo = std::max(upTo, given);
    measured.widths.push_back(upTo - given);
    measured.hyphens.push_back(hyphened > upTo ? hyphened - upTo : 0);
    given = upTo;
  }
  return measured;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static const std::vector<ImageFace> kFaces = Faces();
  static const std::vector<std::uint8_t> kImage = bn::test::FontImageBytes(kFaces);
  bn::fuzz::Input input(data, size);
  const std::uint8_t choices = input.Byte();
  constexpr std::array<std::uint16_t, 4> kStyles = {0, 1, 2, 0xFFFF};
  const std::uint16_t style = kStyles[choices & 3U];
  const bool withFallback = (choices & 4U) == 0;

  // The text, and where it is cut.
  std::string text;
  std::vector<std::uint8_t> ends;
  for (const std::uint8_t byte : input.Rest()) {
    if (byte != 0) {
      if (text.size() < kMostText) {
        text.push_back(static_cast<char>(byte));
      }
    } else if (!text.empty() && ends.size() + 1 < kMostPieces && (ends.empty() || ends.back() != text.size())) {
      ends.push_back(static_cast<std::uint8_t>(text.size()));
    }
  }
  if (ends.empty() || ends.back() != text.size()) {
    ends.push_back(static_cast<std::uint8_t>(text.size()));
  }

  FontImage fonts;
  BN_FUZZ_CHECK(fonts.Open(kImage) == bn::draw::FontStatus::kOk, "the image made here would not open");
  FaceMeasure measure;
  measure.SetFace(0, fonts.FaceAt(0));
  measure.SetFace(1, fonts.FaceAt(1));
  if (withFallback) {
    measure.SetFallback(fonts.FaceAt(2));
  }
  const Setting setting{.own = style == 1 ? &kFaces[1] : &kFaces[0], .fallback = withFallback ? &kFaces[2] : nullptr};

  // A glyph at a time.
  const std::vector<Plain> wanted = setting.Glyphs(text, 0, text.size());
  GlyphRun run(measure, text, style);
  bn::draw::Glyph glyph;
  bool drawn = false;
  for (const Plain& plain : wanted) {
    BN_FUZZ_CHECK(run.At() == plain.begin, "a glyph does not begin where the one before it ended");
    BN_FUZZ_CHECK(run.Next(glyph, drawn), "the glyphs ended before the text did");
    BN_FUZZ_CHECK(run.At() == plain.end, "a glyph stands for other bytes than the plain setting's");
    BN_FUZZ_CHECK(drawn == plain.Drawn(), "a character is drawn that should not be, or the other way round");
    BN_FUZZ_CHECK(!drawn || glyph.advance == plain.Advance(), "a glyph is not the plain setting's");
  }
  BN_FUZZ_CHECK(run.At() == text.size() && !run.Next(glyph, drawn), "the glyphs went on past the text");

  // The pieces.
  const Measured measured = MeasureOf(measure, text, style, ends);
  const Measured plain = PlainMeasureOf(setting, text, ends);
  BN_FUZZ_CHECK(measured.widths == plain.widths, "a piece is not as wide as the plain setting makes it");
  BN_FUZZ_CHECK(measured.hyphens == plain.hyphens, "a hyphen does not come to what the plain setting makes it");

  // Cut nowhere, and cut only between its glyphs: the pieces add up to the
  // whole, which is its glyphs' steps.
  const std::vector<std::uint8_t> uncut = {static_cast<std::uint8_t>(text.size())};
  const std::uint32_t whole = Setting::Width(wanted, nullptr);
  BN_FUZZ_CHECK(MeasureOf(measure, text, style, uncut).widths[0] == whole, "the whole is not its glyphs' steps");
  std::vector<std::uint8_t> between;
  for (const Plain& one : wanted) {
    if (between.size() + 1 < kMostPieces && one.end < text.size()) {
      between.push_back(static_cast<std::uint8_t>(one.end));
    }
  }
  between.push_back(static_cast<std::uint8_t>(text.size()));
  std::uint32_t sum = 0;
  for (const std::uint32_t width : MeasureOf(measure, text, style, between).widths) {
    sum += width;
  }
  BN_FUZZ_CHECK(sum == whole, "the pieces of a stretch cut between its glyphs do not add up to it");
  return 0;
}
