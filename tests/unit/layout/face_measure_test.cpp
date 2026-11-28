#include "layout/face_measure.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/font.h"
#include "font_image.h"
#include "hyphenation/american_english.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/paragraph_setter.h"

namespace {

using bn::draw::FaceRole;
using bn::draw::FontImage;
using bn::draw::FontStatus;
using bn::draw::Glyph;
using bn::layout::FaceMeasure;
using bn::layout::GlyphRun;
using bn::test::Block;
using bn::test::FontImageBytes;
using bn::test::ImageFace;

// A serif whose letters a to z have advances of 40 quarters of a pixel and
// up, each a quarter more than the one before; an italic of it, each letter
// 10 more; and a fallback with a Greek alpha that neither has. So an a is
// 10 pixels, a b 10 and a quarter, a c 10 and a half, a d 10 and three
// quarters, and an e 11.
//
// The serif kerns four pairs, in quarters of a pixel: a u after a q half a
// pixel nearer, a z after a k a quarter nearer, a k after a z a pixel and a
// half further, and a k after a k 15 pixels nearer, which is more than a k
// is wide. The italic kerns a z after a k, by more. The fallback has a
// Greek gamma too, and kerns it after an alpha.
//
// The serif joins letters, trying in this order: two f's and an i, as a
// glyph no character has, of 27 and a half pixels; an f and an i, as the
// glyph of U+FB01, of 20; two f's, as another glyph no character has, of 19
// and a half; and an f and an l, as the glyph of U+FB02, of 21. Not two f's
// and an l. Its fi is a pixel nearer a c after it, and its f a pixel and a
// half nearer the hyphen. The italic joins an f and an i and nothing else,
// and the fallback a Greek delta and an epsilon.
std::vector<std::uint8_t> ThreeFaces() {
  ImageFace serif = bn::test::EmptyFace(0, 0);
  ImageFace italic = bn::test::EmptyFace(0, bn::draw::kItalic);
  for (std::uint32_t letter = 'a'; letter <= 'z'; ++letter) {
    serif.glyphs[letter] = Block(static_cast<std::uint16_t>(40 + letter - 'a'), 8, 10);
    italic.glyphs[letter] = Block(static_cast<std::uint16_t>(50 + letter - 'a'), 8, 10);
  }
  serif.glyphs[' '] = bn::test::Blank(20);
  italic.glyphs[' '] = bn::test::Blank(22);
  serif.glyphs['-'] = Block(24, 5, 2);
  serif.glyphs[0x2010] = Block(26, 5, 2);
  italic.glyphs['-'] = Block(28, 5, 2);
  serif.glyphs[0xE9] = Block(47, 8, 13);
  serif.glyphs[0xFFFD] = Block(60, 10, 12, 2);
  serif.missing = Block(36, 8, 12, 1);
  italic.missing = Block(38, 8, 12, 1);
  serif.kerning[{'q', 'u'}] = -2;
  serif.kerning[{'k', 'z'}] = -1;
  serif.kerning[{'z', 'k'}] = 6;
  serif.kerning[{'k', 'k'}] = -60;
  serif.glyphs[0xFB01] = Block(80, 16, 10);
  serif.glyphs[0xFB02] = Block(84, 16, 10);
  serif.extras = {Block(110, 24, 10), Block(78, 16, 10)};
  serif.ligatures = {{.letters = {'f', 'f', 'i'}, .extra = 0},
                     {.letters = {'f', 'i'}, .glyph = 0xFB01},
                     {.letters = {'f', 'f'}, .extra = 1},
                     {.letters = {'f', 'l'}, .glyph = 0xFB02}};
  serif.kerning[{0xFB01, 'c'}] = -4;
  serif.kerning[{'f', 0x2010}] = -6;
  italic.extras = {Block(90, 16, 10)};
  italic.ligatures = {{.letters = {'f', 'i'}, .extra = 0}};
  italic.kerning[{'k', 'z'}] = -9;
  italic.kerning[{'k', '-'}] = -8;
  ImageFace fallback = bn::test::EmptyFace(3, 0);
  fallback.glyphs[0x3B1] = Block(45, 8, 10);
  fallback.glyphs[0x3B3] = Block(48, 8, 10);
  fallback.kerning[{0x3B1, 0x3B3}] = -8;
  fallback.glyphs[0x3B4] = Block(44, 8, 10);
  fallback.glyphs[0x3B5] = Block(44, 8, 10);
  fallback.extras = {Block(70, 14, 10)};
  fallback.ligatures = {{.letters = {0x3B4, 0x3B5}, .extra = 0}};
  fallback.glyphs['a'] = Block(99, 8, 10);  // Never used: the serif has one.
  return FontImageBytes({serif, italic, fallback});
}

class FaceMeasureTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(fonts_.Open(image_), FontStatus::kOk);
    measure_.SetFace(0, fonts_.Find(FaceRole::kSerif, 0, 20));
    measure_.SetFace(1, fonts_.Find(FaceRole::kSerif, bn::draw::kItalic, 20));
    measure_.SetFallback(fonts_.Find(FaceRole::kFallback, 0, 20));
  }

  // What a stretch of text is measured as: its pieces' widths and what a
  // hyphen after each comes to, in pixels.
  struct Measured {
    std::vector<std::uint32_t> widths;
    std::vector<std::uint32_t> hyphens;
  };

  // `text` cut before each of the bytes `cuts`, and measured.
  Measured Pieces(std::string_view text, const std::vector<std::size_t>& cuts, std::uint16_t style = 0) {
    std::vector<std::uint8_t> ends;
    ends.reserve(cuts.size() + 1);
    for (const std::size_t cut : cuts) {
      ends.push_back(static_cast<std::uint8_t>(cut));
    }
    ends.push_back(static_cast<std::uint8_t>(text.size()));
    std::vector<std::uint16_t> widths(ends.size(), 0xFFFF);
    std::vector<std::uint16_t> hyphens(ends.size(), 0xFFFF);
    measure_.Measure(text, style, ends, widths, hyphens);
    Measured measured;
    measured.widths.reserve(ends.size());
    measured.hyphens.reserve(ends.size());
    for (std::size_t i = 0; i < ends.size(); ++i) {
      EXPECT_EQ(widths[i] % 4, 0) << text << " " << i;
      EXPECT_EQ(hyphens[i] % 4, 0) << text << " " << i;
      measured.widths.push_back(widths[i] / 4U);
      measured.hyphens.push_back(hyphens[i] / 4U);
    }
    return measured;
  }

  // The glyphs `text` is drawn with: each one's advance, and where the next
  // begins; an advance of 0 for a character that is not drawn.
  std::vector<std::pair<std::uint16_t, std::size_t>> Glyphs(std::string_view text, std::uint16_t style = 0) {
    std::vector<std::pair<std::uint16_t, std::size_t>> glyphs;
    GlyphRun run(measure_, text, style);
    Glyph glyph;
    bool drawn = false;
    EXPECT_EQ(run.At(), 0U);
    while (run.Next(glyph, drawn)) {
      glyphs.emplace_back(drawn ? glyph.advance : 0, run.At());
    }
    EXPECT_EQ(run.At(), text.size());
    EXPECT_FALSE(run.Next(glyph, drawn));
    return glyphs;
  }

  // The width of `text` as one piece.
  std::uint16_t Width(std::string_view text, std::uint16_t style = 0) {
    const std::array<std::uint8_t, 1> ends = {static_cast<std::uint8_t>(text.size())};
    std::array<std::uint16_t, 1> widths{};
    measure_.Measure(text, style, ends, widths, hyphens_);
    return widths[0];
  }

  std::vector<std::uint8_t> image_ = ThreeFaces();
  FontImage fonts_;
  FaceMeasure measure_;
  // Where the hyphens of a call go when a test is not about them.
  std::array<std::uint16_t, 8> hyphens_{};
};

TEST_F(FaceMeasureTest, MovesThePenAWholeNumberOfPixelsAfterAGlyphAHalfGoingUp) {
  const auto step = [](std::uint16_t advance) {
    Glyph glyph;
    glyph.advance = advance;
    return FaceMeasure::PenStep(glyph);
  };
  EXPECT_EQ(step(0), 0);
  EXPECT_EQ(step(1), 0);
  EXPECT_EQ(step(2), 1);
  EXPECT_EQ(step(40), 10);
  EXPECT_EQ(step(41), 10);
  EXPECT_EQ(step(42), 11);
  EXPECT_EQ(step(43), 11);
  EXPECT_EQ(step(44), 11);
  EXPECT_EQ(step(0xFFFF), 0x4000);
}

TEST_F(FaceMeasureTest, MeasuresAPieceAsItsGlyphsStepsAddedUpInWholePixels) {
  // "hyph" "en" "ation": in pixels h is 12, y 16, p 14, e 11, n 13, a 10,
  // t 15, i 12, o 14; and a width is four to the pixel.
  const std::array<std::uint8_t, 3> ends = {4, 6, 11};
  std::array<std::uint16_t, 3> widths{};
  measure_.Measure("hyphenation", 0, ends, widths, hyphens_);
  EXPECT_EQ(widths,
            (std::array<std::uint16_t, 3>{4 * (12 + 16 + 14 + 12), 4 * (11 + 13), 4 * (10 + 15 + 12 + 14 + 13)}));
  EXPECT_EQ(measure_.BytesMeasured(), 11U);

  // In another style, the other face's: 12.5, 12.75 and 13 pixels.
  EXPECT_EQ(Width("abc", 1), 4 * (13 + 13 + 13));
  EXPECT_EQ(measure_.BytesMeasured(), 14U);
  EXPECT_EQ(Width(""), 0);
}

TEST_F(FaceMeasureTest, GivesALetterTheSameRoomWhereverItComes) {
  // A c is 10 and a half pixels. Eight of them are 84 pixels added up as
  // they are, and a pen put to the nearest pixel after each would move 11
  // and 10 by turns. Here each is 11, however the word is cut.
  EXPECT_EQ(Width("cccccccc"), 4 * 8 * 11);
  const std::array<std::uint8_t, 8> ends = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<std::uint16_t, 8> widths{};
  measure_.Measure("cccccccc", 0, ends, widths, hyphens_);
  for (const std::uint16_t width : widths) {
    EXPECT_EQ(width, 4 * 11);
  }
  // And a b, a quarter over 10, is 10 each time.
  const std::array<std::uint8_t, 3> uneven = {1, 5, 8};
  std::array<std::uint16_t, 3> pieces{};
  measure_.Measure("bbbbbbbb", 0, uneven, pieces, hyphens_);
  EXPECT_EQ(pieces, (std::array<std::uint16_t, 3>{4 * 10, 4 * 40, 4 * 30}));
}

TEST_F(FaceMeasureTest, KernsALetterWithTheNextBeforeItRoundsTheStep) {
  const auto glyph = [&](std::uint16_t style, std::uint32_t codePoint) {
    Glyph found;
    EXPECT_TRUE(measure_.GlyphFor(style, codePoint, found)) << codePoint;
    return found;
  };
  // A k is 12 and a half pixels: 13 by itself, and 12 before a z, a quarter
  // of a pixel nearer. A kerning of less than half a pixel moves a letter a
  // whole one, or not at all, by what its advance has over a pixel.
  EXPECT_EQ(FaceMeasure::PenStep(glyph(0, 'k')), 13);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'k'), glyph(0, 'z')), 12);
  // A q is 14 pixels, and 13 and a half before a u: 14 either way.
  EXPECT_EQ(FaceMeasure::PenStep(glyph(0, 'q')), 14);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'q'), glyph(0, 'u')), 14);
  // A z is 16 and a quarter, and 17 and three quarters before a k.
  EXPECT_EQ(FaceMeasure::PenStep(glyph(0, 'z')), 16);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'z'), glyph(0, 'k')), 18);
  // The other way round is another pair, and most pairs are not kerned.
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'u'), glyph(0, 'q')), FaceMeasure::PenStep(glyph(0, 'u')));
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'k'), glyph(0, 'a')), 13);
  // A pen is never moved back.
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'k'), glyph(0, 'k')), 0);

  // Another style's face kerns as it does: its k is 15 pixels, and 12 and
  // three quarters before a z.
  EXPECT_EQ(FaceMeasure::PenStep(glyph(1, 'k')), 15);
  EXPECT_EQ(measure_.PenStep(1, glyph(1, 'k'), glyph(1, 'z')), 13);
  // Glyphs of two faces are not kerned, whichever style is asked about.
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'k'), glyph(1, 'z')), 13);
  EXPECT_EQ(measure_.PenStep(1, glyph(1, 'k'), glyph(0, 'z')), 15);
  EXPECT_EQ(measure_.PenStep(1, glyph(0, 'k'), glyph(0, 'z')), 13);

  // What the fallback face lends is kerned by the fallback face: an alpha
  // is 11 and a quarter pixels, and 9 and a quarter before a gamma. But not
  // with a letter of the style's own.
  EXPECT_EQ(FaceMeasure::PenStep(glyph(0, 0x3B1)), 11);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 0x3B1), glyph(0, 0x3B3)), 9);
  EXPECT_EQ(measure_.PenStep(1, glyph(1, 0x3B1), glyph(1, 0x3B3)), 9);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 'k'), glyph(0, 0x3B3)), 13);
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 0x3B1), glyph(0, 'k')), 11);
  // Without a fallback face, neither is: both are the style's glyph 0.
  measure_.SetFallback({});
  EXPECT_EQ(measure_.PenStep(0, glyph(0, 0x3B1), glyph(0, 0x3B3)), FaceMeasure::PenStep(glyph(0, 0x3B1)));
}

TEST_F(FaceMeasureTest, MeasuresAWordWithItsLettersKerned) {
  // The last letter has nothing after it, and its own step.
  EXPECT_EQ(Width("kz"), 4 * (12 + 16));
  EXPECT_EQ(Width("zk"), 4 * (18 + 13));
  EXPECT_EQ(Width("kzk"), 4 * (12 + 18 + 13));
  // In the italic a z is 18 and three quarters pixels.
  EXPECT_EQ(Width("kz", 1), 4 * (13 + 19));
  // The same two letters are as far apart wherever they come in a word.
  EXPECT_EQ(Width("akzkzkza"), 4 * (10 + 12 + 18 + 12 + 18 + 12 + 16 + 10));
  // What the fallback lends among a style's own letters: k, alpha, gamma, z.
  EXPECT_EQ(Width("k\xCE\xB1\xCE\xB3z"), 4 * (13 + 9 + 12 + 16));
}

TEST_F(FaceMeasureTest, KernsAcrossWhereAWordIsCutIntoPieces) {
  // A word cut after every letter: each piece is as wide as its letter's
  // step before the next, so the pieces add up to the word however it is
  // cut, and a pair is as far apart with a cut between as without.
  const std::array<std::uint8_t, 6> ends = {1, 2, 3, 4, 5, 6};
  std::array<std::uint16_t, 6> widths{};
  measure_.Measure("kzkzkz", 0, ends, widths, hyphens_);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 6>{4 * 12, 4 * 18, 4 * 12, 4 * 18, 4 * 12, 4 * 16}));
  EXPECT_EQ(Width("kzkzkz"), 4 * (12 + 18 + 12 + 18 + 12 + 16));
  // Cut in two, with a piece of nothing at the end.
  const std::array<std::uint8_t, 3> uneven = {3, 6, 6};
  std::array<std::uint16_t, 3> pieces{};
  measure_.Measure("kzkzkz", 0, uneven, pieces, hyphens_);
  EXPECT_EQ(pieces, (std::array<std::uint16_t, 3>{4 * (12 + 18 + 12), 4 * (18 + 12 + 16), 0}));
  // And with a piece of nothing in the middle: the letter before it is
  // kerned with the one after.
  const std::array<std::uint8_t, 3> hollow = {1, 1, 2};
  measure_.Measure("kz", 0, hollow, pieces, hyphens_);
  EXPECT_EQ(pieces, (std::array<std::uint16_t, 3>{4 * 12, 0, 4 * 16}));
}

TEST_F(FaceMeasureTest, KernsNothingAcrossWhatIsNotDrawn) {
  // A soft hyphen between a k and a z, and a zero width space: the k is
  // as wide as it is by itself.
  EXPECT_EQ(Width("k\xC2\xADz"), 4 * (13 + 16));
  EXPECT_EQ(Width("k\xE2\x80\x8Bz"), 4 * (13 + 16));
  // And the letters after it are kerned with one another again.
  EXPECT_EQ(Width("k\xC2\xADzk"), 4 * (13 + 18 + 13));
}

TEST_F(FaceMeasureTest, MeasuresACharacterOfSeveralBytesInThePieceItBegins) {
  // "c", "é" and "s": the é is two bytes.
  const std::array<std::uint8_t, 2> ends = {3, 4};
  std::array<std::uint16_t, 2> widths{};
  measure_.Measure("c\xC3\xA9s", 0, ends, widths, hyphens_);
  // 10.5, 11.75 and 14.5 pixels.
  EXPECT_EQ(widths, (std::array<std::uint16_t, 2>{4 * (11 + 12), 4 * 15}));
}

TEST_F(FaceMeasureTest, GivesTheSpaceAndTheHyphenOfAStyle) {
  // Each to the nearest pixel, as a letter is: spaces of 5 and 5.5 pixels.
  EXPECT_EQ(measure_.SpaceWidth(0), 4 * 5);
  EXPECT_EQ(measure_.SpaceWidth(1), 4 * 6);
  // The serif has U+2010, 6.5 pixels, and is given it; the italic has a
  // hyphen-minus only, 7 pixels. After a letter that is not kerned with
  // it, a hyphen comes to itself.
  EXPECT_EQ(Pieces("abc", {}).hyphens, (std::vector<std::uint32_t>{7}));
  EXPECT_EQ(Pieces("abc", {1, 2}).hyphens, (std::vector<std::uint32_t>{7, 7, 7}));
  EXPECT_EQ(Pieces("abc", {}, 1).hyphens, (std::vector<std::uint32_t>{7}));
  // After nothing, too.
  EXPECT_EQ(Pieces("", {}).hyphens, (std::vector<std::uint32_t>{7}));
  Glyph glyph;
  ASSERT_TRUE(measure_.HyphenGlyph(0, glyph));
  EXPECT_EQ(glyph.advance, 26);
  ASSERT_TRUE(measure_.HyphenGlyph(1, glyph));
  EXPECT_EQ(glyph.advance, 28);
}

TEST_F(FaceMeasureTest, DrawsWhatAStyleLacksFromTheFallbackAndThenAsGlyphZero) {
  Glyph glyph;
  // The serif's own a, not the fallback's.
  ASSERT_TRUE(measure_.GlyphFor(0, 'a', glyph));
  EXPECT_EQ(glyph.advance, 40);
  // Alpha is the fallback's alone.
  ASSERT_TRUE(measure_.GlyphFor(0, 0x3B1, glyph));
  EXPECT_EQ(glyph.advance, 45);
  ASSERT_TRUE(measure_.GlyphFor(1, 0x3B1, glyph));
  EXPECT_EQ(glyph.advance, 45);
  // Nobody has a beta: each style's own glyph for what it lacks.
  ASSERT_TRUE(measure_.GlyphFor(0, 0x3B2, glyph));
  EXPECT_EQ(glyph.advance, 36);
  EXPECT_EQ(glyph.index, 0U);
  ASSERT_TRUE(measure_.GlyphFor(1, 0x3B2, glyph));
  EXPECT_EQ(glyph.advance, 38);
  // And they are measured as they are drawn: a, alpha, beta, of 10, 11.25
  // and 9 pixels.
  EXPECT_EQ(Width("a\xCE\xB1\xCE\xB2"), 4 * (10 + 11 + 9));

  // Without a fallback, what the style lacks is its glyph 0.
  measure_.SetFallback({});
  ASSERT_TRUE(measure_.GlyphFor(0, 0x3B1, glyph));
  EXPECT_EQ(glyph.advance, 36);
}

TEST_F(FaceMeasureTest, GivesNoRoomToASoftHyphenOrAZeroWidthSpace) {
  EXPECT_FALSE(FaceMeasure::Drawn(0x00AD));
  EXPECT_FALSE(FaceMeasure::Drawn(0x200B));
  EXPECT_TRUE(FaceMeasure::Drawn('a'));
  Glyph glyph;
  EXPECT_FALSE(measure_.GlyphFor(0, 0x00AD, glyph));
  EXPECT_FALSE(measure_.GlyphFor(0, 0x200B, glyph));
  // "a", a soft hyphen, "b", a zero width space, "c".
  EXPECT_EQ(Width("a\xC2\xAD"
                  "b\xE2\x80\x8B"
                  "c"),
            4 * (10 + 10 + 11));
}

TEST(FaceMeasure, DrawsEveryCharacterButThoseUnicodeSaysAreShownAsNothing) {
  // The first and the last of each run of them, written out a second time.
  const std::vector<std::pair<std::uint32_t, std::uint32_t>> runs = {
      {0x00AD, 0x00AD}, {0x034F, 0x034F}, {0x061C, 0x061C},   {0x115F, 0x1160},   {0x17B4, 0x17B5},   {0x180B, 0x180F},
      {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x206F},   {0x3164, 0x3164},   {0xFE00, 0xFE0F},   {0xFEFF, 0xFEFF},
      {0xFFA0, 0xFFA0}, {0xFFF0, 0xFFF8}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0000, 0xE0FFF},
  };
  for (const auto& [first, last] : runs) {
    EXPECT_FALSE(FaceMeasure::Drawn(first)) << std::hex << first;
    EXPECT_FALSE(FaceMeasure::Drawn(last)) << std::hex << last;
    EXPECT_FALSE(FaceMeasure::Drawn((first + last) / 2)) << std::hex << first;
    EXPECT_TRUE(FaceMeasure::Drawn(first - 1)) << std::hex << first;
    EXPECT_TRUE(FaceMeasure::Drawn(last + 1)) << std::hex << last;
  }
  // 4,174 of them among all the characters there can be, and no more.
  std::size_t notDrawn = 0;
  for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFF; ++codePoint) {
    if (!FaceMeasure::Drawn(codePoint)) {
      ++notDrawn;
    }
  }
  EXPECT_EQ(notDrawn, 4174U);
  // What is beside them in a book is drawn: a no-break space, a dash, the
  // marks of quotation, an ellipsis, an arrow and the replacement character.
  for (const std::uint32_t codePoint : {0x00A0U, 0x2014U, 0x2019U, 0x201CU, 0x2026U, 0x202FU, 0x21A9U, 0xFFFDU}) {
    EXPECT_TRUE(FaceMeasure::Drawn(codePoint)) << std::hex << codePoint;
  }
}

TEST_F(FaceMeasureTest, GivesNoRoomToACharacterThatIsNotDrawn) {
  // A word joiner, U+FEFF, a selector of a symbol's form, a zero width
  // non-joiner, a mark of direction, and a tag, which is four bytes.
  Glyph glyph;
  for (const std::uint32_t codePoint : {0x2060U, 0xFEFFU, 0xFE0EU, 0x200CU, 0x200EU, 0xE0001U}) {
    EXPECT_FALSE(measure_.GlyphFor(0, codePoint, glyph)) << std::hex << codePoint;
    EXPECT_FALSE(measure_.GlyphFor(1, codePoint, glyph)) << std::hex << codePoint;
  }
  // "a", a word joiner, "b", U+FEFF, "c", a selector and a tag.
  const std::string_view text =
      "a\xE2\x81\xA0"
      "b\xEF\xBB\xBF"
      "c\xEF\xB8\x8E\xF3\xA0\x80\x81";
  EXPECT_EQ(Width(text), 4 * (10 + 10 + 11));
  EXPECT_EQ(Glyphs(text), (std::vector<std::pair<std::uint16_t, std::size_t>>{
                              {40, 1}, {0, 4}, {41, 5}, {0, 8}, {42, 9}, {0, 12}, {0, 16}}));
  // One by itself is no width at all, and nor are several.
  EXPECT_EQ(Width("\xE2\x81\xA0"), 0);
  EXPECT_EQ(Width("\xEF\xBB\xBF\xE2\x81\xA0\xEF\xB8\x8E"), 0);
  // A piece that ends before one or after one is as wide as its letters.
  EXPECT_EQ(Pieces("a\xE2\x81\xA0"
                   "b",
                   {1})
                .widths,
            (std::vector<std::uint32_t>{10, 10}));
  EXPECT_EQ(Pieces("a\xE2\x81\xA0"
                   "b",
                   {4})
                .widths,
            (std::vector<std::uint32_t>{10, 10}));
}

TEST_F(FaceMeasureTest, KernsAndJoinsNothingAcrossACharacterThatIsNotDrawn) {
  // A k after a z is a pixel and a half further: 18 pixels and 13, where
  // the z by itself is 16.
  EXPECT_EQ(Width("zk"), 4 * (18 + 13));
  EXPECT_EQ(Width("z\xE2\x81\xA0k"), 4 * (16 + 13));
  EXPECT_EQ(Width("z\xEF\xBB\xBFk"), 4 * (16 + 13));
  // An f and an i are one glyph of 20 pixels, and 11 and 12 with a zero
  // width non-joiner between them, or a word joiner.
  EXPECT_EQ(Width("fi"), 4 * 20);
  EXPECT_EQ(Width("f\xE2\x80\x8Ci"), 4 * (11 + 12));
  EXPECT_EQ(Glyphs("f\xE2\x80\x8Ci"), (std::vector<std::pair<std::uint16_t, std::size_t>>{{45, 1}, {0, 4}, {48, 5}}));
  EXPECT_EQ(Width("f\xE2\x81\xA0i"), 4 * (11 + 12));
  // The letters after one are joined from there: an f, and the glyph for
  // fi.
  EXPECT_EQ(Glyphs("f\xE2\x81\xA0"
                   "fi"),
            (std::vector<std::pair<std::uint16_t, std::size_t>>{{45, 1}, {0, 4}, {80, 6}}));
}

TEST(FaceMeasure, DoesNotDrawSuchACharacterThoughAFaceHasAGlyphForIt) {
  // A serif with a glyph for the word joiner and one for U+FEFF, as some
  // fonts have, and a fallback with one for a selector.
  ImageFace serif = bn::test::EmptyFace(0, 0);
  serif.glyphs['a'] = Block(40, 8, 10);
  serif.glyphs[0x2060] = Block(33, 4, 4);
  serif.glyphs[0xFEFF] = Block(35, 4, 4);
  ImageFace fallback = bn::test::EmptyFace(3, 0);
  fallback.glyphs[0xFE0E] = Block(37, 4, 4);
  const std::vector<std::uint8_t> image = FontImageBytes({serif, fallback});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  FaceMeasure measure;
  measure.SetFace(0, fonts.Find(FaceRole::kSerif, 0, 20));
  measure.SetFallback(fonts.Find(FaceRole::kFallback, 0, 20));

  Glyph glyph;
  ASSERT_TRUE(measure.FaceOf(0).Find(0x2060, glyph));
  EXPECT_EQ(glyph.advance, 33);
  EXPECT_FALSE(measure.GlyphFor(0, 0x2060, glyph));
  EXPECT_FALSE(measure.GlyphFor(0, 0xFEFF, glyph));
  EXPECT_FALSE(measure.GlyphFor(0, 0xFE0E, glyph));
  // "a", a word joiner, U+FEFF, "a", a selector.
  const std::string_view text =
      "a\xE2\x81\xA0\xEF\xBB\xBF"
      "a\xEF\xB8\x8E";
  const std::array<std::uint8_t, 1> ends = {static_cast<std::uint8_t>(text.size())};
  std::array<std::uint16_t, 1> widths{};
  std::array<std::uint16_t, 1> hyphens{};
  measure.Measure(text, 0, ends, widths, hyphens);
  EXPECT_EQ(widths[0], 4 * (10 + 10));
}

TEST_F(FaceMeasureTest, MeasuresAByteThatIsNotUtf8AsTheReplacementCharacter) {
  // A byte that goes on with a character and begins none, a character cut
  // short, and one written the long way round: each byte is one.
  // The replacement character is 15 pixels.
  EXPECT_EQ(Width("a\x80"
                  "b"),
            4 * (10 + 15 + 10));
  EXPECT_EQ(Width("a\xC3"), 4 * (10 + 15));
  EXPECT_EQ(Width("\xC0\xAF"), 4 * (15 + 15));
  // The italic has no replacement character, nor the fallback: its glyph 0,
  // of 9.5 pixels.
  EXPECT_EQ(Width("\xFF", 1), 4 * 10);
}

TEST_F(FaceMeasureTest, GivesAStyleWithNoFaceOfItsOwnStyleZeros) {
  EXPECT_EQ(measure_.FaceOf(2).Style(), 0);
  EXPECT_EQ(Width("abc", 2), 4 * (10 + 10 + 11));
  EXPECT_EQ(measure_.SpaceWidth(7), 4 * 5);
  // A style past the last there can be is style 0 as well.
  measure_.SetFace(FaceMeasure::kMostStyles, fonts_.Find(FaceRole::kSerif, bn::draw::kItalic, 20));
  EXPECT_EQ(Width("abc", static_cast<std::uint16_t>(FaceMeasure::kMostStyles)), 4 * (10 + 10 + 11));
  EXPECT_EQ(Width("abc", 0xFFFF), 4 * (10 + 10 + 11));
}

TEST_F(FaceMeasureTest, MeasuresNothingWithNoFaces) {
  FaceMeasure none;
  const std::array<std::uint8_t, 1> ends = {3};
  std::array<std::uint16_t, 1> widths = {7};
  std::array<std::uint16_t, 1> hyphens = {7};
  none.Measure("abc", 0, ends, widths, hyphens);
  EXPECT_EQ(widths[0], 0);
  EXPECT_EQ(hyphens[0], 0);
  EXPECT_EQ(none.SpaceWidth(0), 0);
  Glyph glyph;
  EXPECT_FALSE(none.GlyphFor(0, 'a', glyph));
  EXPECT_FALSE(none.HyphenGlyph(0, glyph));
}

TEST_F(FaceMeasureTest, HoldsAPieceTooWideForItsNumberAtTheMostPixelsThereAre) {
  // 255 letters of 75 pixels each are more than 16 bits hold in quarters.
  ImageFace wide;
  wide.glyphs['w'] = Block(300, 60, 10);
  const std::vector<std::uint8_t> image = FontImageBytes({wide});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  FaceMeasure measure;
  measure.SetFace(0, fonts.FaceAt(0));
  const std::string text(255, 'w');
  const std::array<std::uint8_t, 2> ends = {200, 255};
  std::array<std::uint16_t, 2> widths{};
  measure.Measure(text, 0, ends, widths, hyphens_);
  EXPECT_EQ(widths[0], 60000);
  EXPECT_EQ(widths[1], 55 * 300);
  const std::array<std::uint8_t, 1> whole = {255};
  std::array<std::uint16_t, 1> one{};
  measure.Measure(text, 0, whole, one, hyphens_);
  EXPECT_EQ(one[0], 4 * FaceMeasure::kMostPixels);
  EXPECT_EQ(one[0], 0xFFFC);
}

TEST_F(FaceMeasureTest, StopsAtTheEndOfTheTextWhateverThePiecesSay) {
  const std::array<std::uint8_t, 3> ends = {2, 200, 255};
  std::array<std::uint16_t, 3> widths = {1, 1, 1};
  measure_.Measure("abc", 0, ends, widths, hyphens_);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 3>{4 * (10 + 10), 4 * 11, 0}));
  // And fills no more widths than it is given room for.
  std::array<std::uint16_t, 1> one = {1};
  measure_.Measure("abc", 0, ends, one, hyphens_);
  EXPECT_EQ(one[0], 4 * (10 + 10));
  // What is past the last piece's end is not looked at: a k is not kerned
  // with a z there, nor an f joined with an i.
  const std::array<std::uint8_t, 1> first = {1};
  measure_.Measure("kz", 0, first, one, hyphens_);
  EXPECT_EQ(one[0], 4 * 13);
  measure_.Measure("fi", 0, first, one, hyphens_);
  EXPECT_EQ(one[0], 4 * 11);
  // Nor is a hyphen given where there is no room for one.
  std::array<std::uint16_t, 3> three = {1, 1, 1};
  std::array<std::uint16_t, 3> few = {1, 1, 1};
  measure_.Measure("abc", 0, ends, three, std::span<std::uint16_t>(few).first(1));
  EXPECT_EQ(few, (std::array<std::uint16_t, 3>{4 * 7, 1, 1}));
}

TEST_F(FaceMeasureTest, KernsTheLastLetterBeforeAHyphenWithTheHyphenAndNotWithWhatFollows) {
  // "kz" and "k": on one line the z is 18 pixels before the k, and that is
  // its piece's. Where a line ends between them the z has the hyphen after
  // it, which it is not kerned with: its own 16, and the hyphen's 7. So the
  // hyphen there comes to 5: the 23 the line has after its k, less the 18.
  const Measured apart = Pieces("kzk", {2});
  EXPECT_EQ(apart.widths, (std::vector<std::uint32_t>{12 + 18, 13}));
  EXPECT_EQ(apart.hyphens, (std::vector<std::uint32_t>{16 + 7 - 18, 7}));
  // "af" and "ar": an f is 11 pixels, and 10 before a hyphen, a pixel and a
  // half nearer. The hyphen comes to 6 there, and to as much at the end of
  // the stretch.
  const Measured nearer = Pieces("afar", {2});
  EXPECT_EQ(nearer.widths, (std::vector<std::uint32_t>{10 + 11, 10 + 14}));
  EXPECT_EQ(nearer.hyphens, (std::vector<std::uint32_t>{6, 7}));
  EXPECT_EQ(Pieces("af", {}).hyphens, (std::vector<std::uint32_t>{6}));
  // Nothing is kerned across what is not drawn, a hyphen no more than a
  // letter: an f, a soft hyphen, and the end of the stretch.
  EXPECT_EQ(Pieces("af\xC2\xAD", {}).hyphens, (std::vector<std::uint32_t>{7}));
}

TEST_F(FaceMeasureTest, GoesThroughAStretchAGlyphAtATimeWithItsLettersJoined) {
  using G = std::vector<std::pair<std::uint16_t, std::size_t>>;
  // An o, the glyph for two f's and an i, a c and an e.
  EXPECT_EQ(Glyphs("office"), (G{{54, 1}, {110, 4}, {42, 5}, {44, 6}}));
  EXPECT_EQ(Glyphs("fi"), (G{{80, 2}}));
  EXPECT_EQ(Glyphs("off"), (G{{54, 1}, {78, 3}}));
  EXPECT_EQ(Glyphs("fl"), (G{{84, 2}}));
  // Two f's and an l are the glyph for two f's and an l by itself: the
  // first of the face's ligatures that fits is taken, and what follows it
  // is looked at afresh.
  EXPECT_EQ(Glyphs("ffl"), (G{{78, 2}, {51, 3}}));
  // Three f's and an i are two f's and then an f and an i.
  EXPECT_EQ(Glyphs("fffi"), (G{{78, 2}, {80, 4}}));
  EXPECT_EQ(Glyphs("fif"), (G{{80, 2}, {45, 3}}));
  EXPECT_EQ(Glyphs("f"), (G{{45, 1}}));
  EXPECT_EQ(Glyphs(""), G{});
  // Nothing is joined across a character that is not drawn.
  EXPECT_EQ(Glyphs("f\xC2\xADi"), (G{{45, 1}, {0, 3}, {48, 4}}));
  EXPECT_EQ(Glyphs("f\xE2\x80\x8B"
                   "f"),
            (G{{45, 1}, {0, 4}, {45, 5}}));
  // Nor a letter with what is not one: a byte that is no character is the
  // replacement character's glyph.
  EXPECT_EQ(Glyphs("f\xFFi"), (G{{45, 1}, {60, 2}, {48, 3}}));
  // The character U+FB01 itself is its own glyph.
  EXPECT_EQ(Glyphs("\xEF\xAC\x81"), (G{{80, 3}}));
}

TEST_F(FaceMeasureTest, JoinsLettersAsTheFaceThatDrawsThemDoes) {
  using G = std::vector<std::pair<std::uint16_t, std::size_t>>;
  // The italic joins an f and an i, and not two f's.
  EXPECT_EQ(Glyphs("fi", 1), (G{{90, 2}}));
  EXPECT_EQ(Glyphs("ffi", 1), (G{{55, 1}, {90, 3}}));
  EXPECT_TRUE(measure_.Joins(0));
  EXPECT_TRUE(measure_.Joins(1));
  // What the fallback face lends is joined as the fallback joins it, in
  // either style: a delta and an epsilon.
  EXPECT_EQ(Glyphs("\xCE\xB4\xCE\xB5"), (G{{70, 4}}));
  EXPECT_EQ(Glyphs("\xCE\xB4\xCE\xB5", 1), (G{{70, 4}}));
  EXPECT_EQ(Glyphs("\xCE\xB5\xCE\xB4"), (G{{44, 2}, {44, 4}}));
  // A letter of the style's own and one the fallback lent are not joined:
  // the fallback has an f and an i of its own, which it would join, and
  // neither is used.
  EXPECT_EQ(Glyphs("f\xCE\xB5"), (G{{45, 1}, {44, 3}}));
  // Without the fallback face its letters are the style's glyph for what
  // it lacks, and are not joined.
  measure_.SetFallback({});
  EXPECT_EQ(Glyphs("\xCE\xB4\xCE\xB5"), (G{{36, 2}, {36, 4}}));
  // A face that joins nothing: the serif's letters with its ligatures left
  // out.
  ImageFace plain;
  plain.glyphs['f'] = Block(45, 8, 10);
  plain.glyphs['i'] = Block(48, 8, 10);
  const std::vector<std::uint8_t> image = FontImageBytes({plain});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  FaceMeasure measure;
  measure.SetFace(0, fonts.FaceAt(0));
  EXPECT_FALSE(measure.Joins(0));
  GlyphRun run(measure, "fi", 0);
  Glyph glyph;
  bool drawn = false;
  ASSERT_TRUE(run.Next(glyph, drawn));
  EXPECT_EQ(glyph.advance, 45);
  EXPECT_EQ(run.At(), 1U);
  // With a fallback face that joins, what that lends is joined still.
  measure.SetFallback(fonts_.Find(FaceRole::kFallback, 0, 20));
  EXPECT_TRUE(measure.Joins(0));
  GlyphRun lent(measure, "\xCE\xB4\xCE\xB5", 0);
  ASSERT_TRUE(lent.Next(glyph, drawn));
  EXPECT_EQ(glyph.advance, 70);
  EXPECT_EQ(lent.At(), 4U);
}

TEST_F(FaceMeasureTest, FindsAStylesHyphenAndWhatItJoinsAgainWhenAFaceIsSet) {
  // Each style has its own hyphen, whichever is measured first. The serif's
  // is 7 pixels after a k. The italic's is as wide, and the italic kerns it
  // two pixels nearer its k, which it does with no glyph of another face.
  EXPECT_EQ(Pieces("ak", {}, 0).hyphens, (std::vector<std::uint32_t>{7}));
  EXPECT_EQ(Pieces("ak", {}, 1).hyphens, (std::vector<std::uint32_t>{5}));
  EXPECT_EQ(Pieces("ak", {}, 0).hyphens, (std::vector<std::uint32_t>{7}));
  // The italic's hyphen is its hyphen-minus, which it kerns no f with: 7
  // pixels after an f of 14. The serif's is a pixel and a half nearer its
  // f.
  EXPECT_EQ(Pieces("af", {}, 1).hyphens, (std::vector<std::uint32_t>{7}));
  EXPECT_EQ(Width("ff", 1), 4 * (14 + 14));
  // With the serif set for that style, text in it is the serif's: its
  // hyphen, and its two f's joined.
  measure_.SetFace(1, fonts_.Find(FaceRole::kSerif, 0, 20));
  EXPECT_EQ(Pieces("af", {}, 1).hyphens, (std::vector<std::uint32_t>{6}));
  EXPECT_EQ(Width("ff", 1), 4 * 20);
  // And the fallback's: a delta and an epsilon are joined while there is a
  // fallback face, and are two of the style's glyph for what it lacks when
  // there is none.
  EXPECT_EQ(Width("\xCE\xB4\xCE\xB5"), 4 * 18);
  measure_.SetFallback({});
  EXPECT_EQ(Width("\xCE\xB4\xCE\xB5"), 4 * (9 + 9));
  measure_.SetFallback(fonts_.Find(FaceRole::kFallback, 0, 20));
  EXPECT_EQ(Width("\xCE\xB4\xCE\xB5"), 4 * 18);
  // The marks of the glyphs that may begin a ligature: the serif's f and
  // the fallback's delta, and for the italic its own f and the delta.
  Glyph f;
  Glyph delta;
  ASSERT_TRUE(measure_.GlyphFor(0, 'f', f));
  ASSERT_TRUE(measure_.GlyphFor(0, 0x3B4, delta));
  EXPECT_EQ(measure_.JoinMarks(0), (std::uint64_t{1} << f.index) | (std::uint64_t{1} << delta.index));
  EXPECT_EQ(measure_.JoinMarks(7), measure_.JoinMarks(0));
  measure_.SetFallback({});
  EXPECT_EQ(measure_.JoinMarks(0), std::uint64_t{1} << f.index);
}

TEST_F(FaceMeasureTest, MeasuresAWordWithItsLettersJoined) {
  // In pixels an o is 14, the glyph for two f's and an i 28, a c and an e
  // 11 each.
  EXPECT_EQ(Width("office"), 4 * (14 + 28 + 11 + 11));
  // The glyph for an f and an i is 20, and 19 before a c.
  EXPECT_EQ(Width("fi"), 4 * 20);
  EXPECT_EQ(Width("fic"), 4 * (19 + 11));
  // Two f's 20, an l 13, an i 12, an n 13.
  EXPECT_EQ(Width("offline"), 4 * (14 + 20 + 13 + 12 + 13 + 11));
  // A ligature is counted in the piece it begins in when no line ends
  // inside it: the pieces add up to the word.
  const Measured cut = Pieces("office", {1, 4});
  EXPECT_EQ(cut.widths, (std::vector<std::uint32_t>{14, 28, 11 + 11}));
  // In the italic, where an f is 14 pixels and the glyph for fi 22 and a
  // half.
  EXPECT_EQ(Width("ffi", 1), 4 * (14 + 23));
}

TEST_F(FaceMeasureTest, CountsAWordCutInsideALigatureAsEachOfItsLinesIsDrawn) {
  // "of" and "fice". Whole, the word is 64 pixels: 14, 28, 11 and 11. With
  // a line's end between its f's, the second line is the glyph for an f and
  // an i, 19 before the c, and the c and the e: 41, which is what the
  // second piece is given, and the first has the other 23. The first line
  // is an o, an f of 10 before its hyphen, and the hyphen's 7: 31, so the
  // hyphen there comes to 8 more than the piece.
  const Measured between = Pieces("office", {2});
  EXPECT_EQ(between.widths, (std::vector<std::uint32_t>{23, 41}));
  EXPECT_EQ(between.hyphens, (std::vector<std::uint32_t>{8, 7}));
  // "off" and "ice": the second line is three letters, 12, 11 and 11, and
  // the first an o, the glyph for two f's, 20, and the hyphen.
  const Measured after = Pieces("office", {3});
  EXPECT_EQ(after.widths, (std::vector<std::uint32_t>{64 - 34, 34}));
  EXPECT_EQ(after.hyphens, (std::vector<std::uint32_t>{14 + 20 + 7 - 30, 7}));
  // Cut at both places: the pieces from either cut on are as wide as the
  // text from there on is drawn.
  const Measured both = Pieces("office", {2, 3});
  EXPECT_EQ(both.widths, (std::vector<std::uint32_t>{23, 7, 34}));
  EXPECT_EQ(both.hyphens, (std::vector<std::uint32_t>{8, 11, 7}));
  // A word that begins inside the ligature it would be part of.
  EXPECT_EQ(Pieces("ffi", {1}).widths, (std::vector<std::uint32_t>{28 - 20, 20}));
  EXPECT_EQ(Pieces("ffi", {1}).hyphens, (std::vector<std::uint32_t>{10 + 7 - 8, 7}));
}

TEST_F(FaceMeasureTest, FindsTheGlyphsAfterACutAfreshUntilTheyFallInWithTheWholeWords) {
  // "of" and "fline". Whole, it is an o, the glyph for two f's, and then an
  // l, an i, an n and an e: 83 pixels. From the cut on it is the glyph for
  // an f and an l, 21, which reaches a letter past where the two f's ended,
  // and the same i, n and e: 57.
  const Measured cut = Pieces("offline", {2});
  EXPECT_EQ(cut.widths, (std::vector<std::uint32_t>{83 - 57, 57}));
  EXPECT_EQ(cut.hyphens, (std::vector<std::uint32_t>{14 + 10 + 7 - 26, 7}));
  // Nine f's are four glyphs for two and an f, 91 pixels, and from the
  // first cut on four glyphs for two: the two never fall in until the end.
  const Measured nine = Pieces("fffffffff", {1});
  EXPECT_EQ(nine.widths, (std::vector<std::uint32_t>{91 - 80, 80}));
}

TEST_F(FaceMeasureTest, GivesUpAfterSoManyGlyphsAndCountsTheTextAfterTheCutNoNarrowerThanItIs) {
  // Thirty f's are fifteen glyphs for two, 300 pixels. From a cut after the
  // first they are fourteen and an f, 291, and the two do not fall in
  // before the end: after kMostGlyphsApart glyphs the ligature the cut is
  // in is counted whole after it, and the piece before the cut has nothing.
  // Its hyphen still makes the line that ends there as wide as it is drawn:
  // an f and a hyphen.
  const std::string text(30, 'f');
  const Measured cut = Pieces(text, {1});
  EXPECT_EQ(cut.widths, (std::vector<std::uint32_t>{0, 300}));
  EXPECT_EQ(cut.hyphens, (std::vector<std::uint32_t>{10 + 7, 7}));
  // Cut between every two letters, the pieces still add up to the whole.
  std::vector<std::size_t> every;
  every.reserve(text.size());
  for (std::size_t at = 1; at < text.size(); ++at) {
    every.push_back(at);
  }
  const Measured all = Pieces(text, every);
  std::uint32_t sum = 0;
  for (const std::uint32_t width : all.widths) {
    sum += width;
  }
  EXPECT_EQ(sum, 300U);
  // And a cut that is not inside a ligature costs nothing more: after two
  // f's, the rest is as it is in the whole.
  EXPECT_EQ(Pieces(text, {2}).widths, (std::vector<std::uint32_t>{20, 280}));
  // Nine f's fall in at the end after eight glyphs, which is as many as
  // are gone through; ten would take nine, and are given up.
  EXPECT_EQ(Pieces("ffffffffff", {1}).widths, (std::vector<std::uint32_t>{0, 100}));
  // What comes before the ligature is the piece's all the same: an o of 14
  // pixels before the thirty f's.
  const Measured after = Pieces("o" + text, {2});
  EXPECT_EQ(after.widths, (std::vector<std::uint32_t>{14, 300}));
  EXPECT_EQ(after.hyphens, (std::vector<std::uint32_t>{10 + 7, 7}));
}

// The serif's letters set the plain way, sharing nothing with the measure:
// a glyph is a number, its letter's or one made up for a ligature, with the
// bytes of the text it stands for.
struct PlainGlyph {
  std::uint32_t glyph = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
  bool operator==(const PlainGlyph&) const = default;
};

constexpr std::uint32_t kPlainFfi = 1;
constexpr std::uint32_t kPlainFf = 2;
constexpr std::uint32_t kPlainFi = 0xFB01;
constexpr std::uint32_t kPlainFl = 0xFB02;
constexpr std::uint32_t kPlainHyphen = 0x2010;

std::uint32_t PlainAdvance(std::uint32_t glyph) {
  switch (glyph) {
    case kPlainFfi:
      return 110;
    case kPlainFf:
      return 78;
    case kPlainFi:
      return 80;
    case kPlainFl:
      return 84;
    case kPlainHyphen:
      return 26;
    default:
      return 40 + glyph - 'a';
  }
}

int PlainKerning(std::uint32_t first, std::uint32_t second) {
  static const std::map<std::pair<std::uint32_t, std::uint32_t>, int> kPairs = {
      {{'q', 'u'}, -2},  {{'k', 'z'}, -1},      {{'z', 'k'}, 6},
      {{'k', 'k'}, -60}, {{kPlainFi, 'c'}, -4}, {{'f', kPlainHyphen}, -6}};
  const auto found = kPairs.find({first, second});
  return found == kPairs.end() ? 0 : found->second;
}

// The glyphs of `text` from byte `from` up to `end`, set by itself.
std::vector<PlainGlyph> PlainGlyphs(std::string_view text, std::size_t from, std::size_t end) {
  std::vector<PlainGlyph> glyphs;
  std::size_t at = from;
  while (at < end) {
    const std::string_view rest = text.substr(at, end - at);
    if (rest.starts_with("ffi")) {
      glyphs.push_back({kPlainFfi, at, at + 3});
    } else if (rest.starts_with("fi")) {
      glyphs.push_back({kPlainFi, at, at + 2});
    } else if (rest.starts_with("ff")) {
      glyphs.push_back({kPlainFf, at, at + 2});
    } else if (rest.starts_with("fl")) {
      glyphs.push_back({kPlainFl, at, at + 2});
    } else {
      glyphs.push_back({static_cast<std::uint32_t>(rest[0]), at, at + 1});
    }
    at = glyphs.back().end;
  }
  return glyphs;
}

// How many pixels `glyphs` are across, with a hyphen after them or not:
// each glyph's advance and its kerning with what follows it, to the nearest
// pixel and never less than nothing.
std::uint32_t PlainWidth(const std::vector<PlainGlyph>& glyphs, bool hyphen) {
  std::uint32_t pixels = 0;
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    int quarters = static_cast<int>(PlainAdvance(glyphs[i].glyph));
    if (i + 1 < glyphs.size()) {
      quarters += PlainKerning(glyphs[i].glyph, glyphs[i + 1].glyph);
    } else if (hyphen) {
      quarters += PlainKerning(glyphs[i].glyph, kPlainHyphen);
    }
    pixels += static_cast<std::uint32_t>(std::max(quarters, 0) + 2) / 4U;
  }
  return pixels + (hyphen ? (PlainAdvance(kPlainHyphen) + 2U) / 4U : 0U);
}

TEST_F(FaceMeasureTest, CountsEveryLineAWordCanBeBrokenIntoAsItIsDrawn) {
  // Words of the letters that are joined and kerned and a few that are
  // not, cut at places taken at random, and each held to the plain setting
  // of the text a line would have.
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same words every run, so that a failure can be found again.
  std::mt19937 random(20261127);
  const std::string_view letters = "fffiilkzoc";
  int middles = 0;
  int insideLigatures = 0;
  for (int round = 0; round < 3000; ++round) {
    const std::size_t length = 2 + (random() % 8);
    std::string word;
    for (std::size_t i = 0; i < length; ++i) {
      const std::size_t which = random() % letters.size();
      word += letters[which];
    }
    std::vector<std::size_t> cuts;
    for (std::size_t at = 1; at < length; ++at) {
      const bool cut = random() % 2 == 0;
      if (cut) {
        cuts.push_back(at);
      }
    }
    const Measured measured = Pieces(word, cuts);
    // Where each piece begins and ends.
    std::vector<std::size_t> edges = {0};
    edges.insert(edges.end(), cuts.begin(), cuts.end());
    edges.push_back(length);
    const std::size_t pieces = edges.size() - 1;
    const std::vector<PlainGlyph> whole = PlainGlyphs(word, 0, length);
    for (const PlainGlyph& glyph : whole) {
      for (const std::size_t cut : cuts) {
        insideLigatures += cut > glyph.begin && cut < glyph.end ? 1 : 0;
      }
    }
    for (std::size_t first = 0; first < pieces; ++first) {
      std::uint32_t sum = 0;
      for (std::size_t last = first; last < pieces; ++last) {
        sum += measured.widths[last];
        const std::vector<PlainGlyph> line = PlainGlyphs(word, edges[first], edges[last + 1]);
        const std::string what =
            word + " from " + std::to_string(edges[first]) + " to " + std::to_string(edges[last + 1]);
        if (last + 1 == pieces) {
          // To the end of the word, from any cut: the pieces by themselves.
          EXPECT_EQ(sum, PlainWidth(line, false)) << what;
        }
        if (first == 0) {
          // From the start of the word to any cut, with a hyphen.
          EXPECT_EQ(sum + measured.hyphens[last], PlainWidth(line, true)) << what;
          continue;
        }
        // A middle: exact where it has a glyph that is the same in the
        // whole word, in the text from its first cut on, and in the text
        // up to its second.
        const std::vector<PlainGlyph> fromFirst = PlainGlyphs(word, edges[first], length);
        const std::vector<PlainGlyph> toLast = PlainGlyphs(word, 0, edges[last + 1]);
        bool unchanged = false;
        for (const PlainGlyph& glyph : whole) {
          unchanged = unchanged || (glyph.begin >= edges[first] && glyph.end <= edges[last + 1] &&
                                    std::find(fromFirst.begin(), fromFirst.end(), glyph) != fromFirst.end() &&
                                    std::find(toLast.begin(), toLast.end(), glyph) != toLast.end());
        }
        if (unchanged) {
          ++middles;
          EXPECT_EQ(sum + measured.hyphens[last], PlainWidth(line, true)) << what;
        }
      }
    }
  }
  // The words did have cuts inside their ligatures, and middles to hold.
  EXPECT_GT(insideLigatures, 1000);
  EXPECT_GT(middles, 1000);
}

// The lines of a paragraph set with it, each word's place and width.
class Lines final : public bn::layout::TextLineSink {
 public:
  void OnLine(const bn::layout::TextLine& line) noexcept override { lines.push_back(line); }
  std::vector<bn::layout::TextLine> lines;
};

TEST_F(FaceMeasureTest, SetsAParagraphWhoseLinesAreAsWideAsTheirGlyphs) {
  std::array<bn::layout::LineBreaker::Node, 64> nodes{};
  std::array<bn::layout::ParagraphSetter::Record, 256> records{};
  Lines sink;
  bn::layout::ParagraphSetter setter(nodes, records);
  const std::string_view text = "abc def ghi jkl mno pqr stu vwx";
  // Room for three words and their spaces, in quarters, and not four.
  const auto width = static_cast<std::uint16_t>(Width("abc") + 20 + Width("def") + 20 + Width("ghi") + 30);
  ASSERT_EQ(setter.Begin({.widths = {.first = {}, .rest = width}, .hyphenPenalty = 0}, measure_, sink),
            bn::layout::ParagraphStatus::kOk);
  std::size_t given = 0;
  for (int calls = 0; given < text.size() && calls < 1000; ++calls) {
    given += setter.AddText(text.substr(given), 0);
  }
  ASSERT_EQ(given, text.size());
  bool finished = false;
  for (int calls = 0; !finished && calls < 1000; ++calls) {
    finished = setter.Finish();
  }
  ASSERT_TRUE(finished);
  // The letters widen through the alphabet: three words, then two, two and one.
  ASSERT_EQ(sink.lines.size(), 4U);
  for (const bn::layout::TextLine& line : sink.lines) {
    // A line's natural width is its words' glyphs and the spaces between,
    // each a whole number of pixels: its advance and half a pixel, less
    // what is then over a pixel.
    const std::string_view words = text.substr(line.begin, line.end - line.begin);
    std::uint32_t wanted = 0;
    for (const char c : words) {
      const std::uint32_t advance = c == ' ' ? 20U : 40U + static_cast<std::uint32_t>(c - 'a');
      wanted += ((advance + 2U) / 4U) * 4U;
    }
    EXPECT_EQ(line.width, wanted) << words;
    EXPECT_EQ(line.width % 4U, 0U) << words;
    EXPECT_LE(line.width, width) << words;
  }
}

TEST_F(FaceMeasureTest, SetsAParagraphOfJoinedLettersInLinesAsWideAsTheyAreDrawn) {
  // Words the hyphenator breaks between two f's, set at many widths, none
  // too narrow for the longest of them: every line is as wide as its text
  // is set the plain way, by itself, with its hyphen if it has one.
  bn::layout::Hyphenator hyphenator;
  ASSERT_EQ(hyphenator.Open(bn::hyphenation::AmericanEnglish().table), bn::layout::HyphenStatus::kOk);
  std::string text;
  for (int times = 0; times < 6; ++times) {
    text +=
        "the official offered coffee in the difficult office and a waffle to the staff of an affluent "
        "sheriff whose efficient kazoo baffled a zinc fiddle ";
  }
  text.pop_back();
  std::array<bn::layout::LineBreaker::Node, 256> nodes{};
  std::array<bn::layout::ParagraphSetter::Record, 768> records{};
  int hyphened = 0;
  int brokenInALigature = 0;
  for (std::uint16_t pixels = 130; pixels < 330; pixels += 7) {
    Lines sink;
    bn::layout::ParagraphSetter setter(nodes, records);
    setter.UseHyphenator(&hyphenator);
    const auto width = static_cast<std::uint16_t>(pixels * 4);
    ASSERT_EQ(setter.Begin({.widths = {.first = {}, .rest = width}, .hyphenPenalty = 0}, measure_, sink),
              bn::layout::ParagraphStatus::kOk);
    std::size_t given = 0;
    for (int calls = 0; given < text.size() && calls < 100000; ++calls) {
      given += setter.AddText(std::string_view(text).substr(given), 0);
    }
    ASSERT_EQ(given, text.size());
    bool finished = false;
    for (int calls = 0; !finished && calls < 100000; ++calls) {
      finished = setter.Finish();
    }
    ASSERT_TRUE(finished);
    EXPECT_EQ(setter.LastResortCuts(), 0U) << pixels;
    for (const bn::layout::TextLine& line : sink.lines) {
      std::uint32_t wanted = 0;
      std::size_t at = line.begin;
      while (at < line.end) {
        if (text[at] == ' ') {
          wanted += 5;
          ++at;
          continue;
        }
        std::size_t end = at;
        while (end < line.end && text[end] != ' ') {
          ++end;
        }
        wanted += PlainWidth(PlainGlyphs(text, at, end), line.hyphen && end == line.end);
        at = end;
      }
      EXPECT_EQ(line.width, wanted * 4U) << pixels << ": " << text.substr(line.begin, line.end - line.begin);
      EXPECT_LE(line.width, width) << pixels;
      hyphened += line.hyphen ? 1 : 0;
      // A line that ends between two f's, or between an f and an i that
      // the whole word draws as one glyph.
      if (line.hyphen && text[line.end - 1] == 'f' && (text[line.end] == 'f' || text[line.end] == 'i')) {
        ++brokenInALigature;
      }
    }
  }
  EXPECT_GT(hyphened, 100);
  EXPECT_GT(brokenInALigature, 20);
}

}  // namespace
