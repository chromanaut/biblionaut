#include "layout/face_measure.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "draw/font.h"
#include "font_image.h"
#include "layout/line_breaker.h"
#include "layout/paragraph_setter.h"

namespace {

using bn::draw::FaceRole;
using bn::draw::FontImage;
using bn::draw::FontStatus;
using bn::draw::Glyph;
using bn::layout::FaceMeasure;
using bn::test::Block;
using bn::test::FontImageBytes;
using bn::test::ImageFace;

// A serif whose letters a to z have advances of 40 quarters of a pixel and
// up, each a quarter more than the one before; an italic of it, each letter
// 10 more; and a fallback with a Greek alpha that neither has. So an a is
// 10 pixels, a b 10 and a quarter, a c 10 and a half, a d 10 and three
// quarters, and an e 11.
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
  ImageFace fallback = bn::test::EmptyFace(3, 0);
  fallback.glyphs[0x3B1] = Block(45, 8, 10);
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

  // The width of `text` as one piece.
  std::uint16_t Width(std::string_view text, std::uint16_t style = 0) {
    const std::array<std::uint8_t, 1> ends = {static_cast<std::uint8_t>(text.size())};
    std::array<std::uint16_t, 1> widths{};
    measure_.Measure(text, style, ends, widths);
    return widths[0];
  }

  std::vector<std::uint8_t> image_ = ThreeFaces();
  FontImage fonts_;
  FaceMeasure measure_;
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
  measure_.Measure("hyphenation", 0, ends, widths);
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
  measure_.Measure("cccccccc", 0, ends, widths);
  for (const std::uint16_t width : widths) {
    EXPECT_EQ(width, 4 * 11);
  }
  // And a b, a quarter over 10, is 10 each time.
  const std::array<std::uint8_t, 3> uneven = {1, 5, 8};
  std::array<std::uint16_t, 3> pieces{};
  measure_.Measure("bbbbbbbb", 0, uneven, pieces);
  EXPECT_EQ(pieces, (std::array<std::uint16_t, 3>{4 * 10, 4 * 40, 4 * 30}));
}

TEST_F(FaceMeasureTest, MeasuresACharacterOfSeveralBytesInThePieceItBegins) {
  // "c", "é" and "s": the é is two bytes.
  const std::array<std::uint8_t, 2> ends = {3, 4};
  std::array<std::uint16_t, 2> widths{};
  measure_.Measure("c\xC3\xA9s", 0, ends, widths);
  // 10.5, 11.75 and 14.5 pixels.
  EXPECT_EQ(widths, (std::array<std::uint16_t, 2>{4 * (11 + 12), 4 * 15}));
}

TEST_F(FaceMeasureTest, GivesTheSpaceAndTheHyphenOfAStyle) {
  // Each to the nearest pixel, as a letter is: spaces of 5 and 5.5 pixels.
  EXPECT_EQ(measure_.SpaceWidth(0), 4 * 5);
  EXPECT_EQ(measure_.SpaceWidth(1), 4 * 6);
  // The serif has U+2010, 6.5 pixels, and is given it; the italic has a
  // hyphen-minus only, 7 pixels.
  EXPECT_EQ(measure_.HyphenWidth(0), 4 * 7);
  EXPECT_EQ(measure_.HyphenWidth(1), 4 * 7);
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
  none.Measure("abc", 0, ends, widths);
  EXPECT_EQ(widths[0], 0);
  EXPECT_EQ(none.SpaceWidth(0), 0);
  EXPECT_EQ(none.HyphenWidth(0), 0);
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
  measure.Measure(text, 0, ends, widths);
  EXPECT_EQ(widths[0], 60000);
  EXPECT_EQ(widths[1], 55 * 300);
  const std::array<std::uint8_t, 1> whole = {255};
  std::array<std::uint16_t, 1> one{};
  measure.Measure(text, 0, whole, one);
  EXPECT_EQ(one[0], 4 * FaceMeasure::kMostPixels);
  EXPECT_EQ(one[0], 0xFFFC);
}

TEST_F(FaceMeasureTest, StopsAtTheEndOfTheTextWhateverThePiecesSay) {
  const std::array<std::uint8_t, 3> ends = {2, 200, 255};
  std::array<std::uint16_t, 3> widths = {1, 1, 1};
  measure_.Measure("abc", 0, ends, widths);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 3>{4 * (10 + 10), 4 * 11, 0}));
  // And fills no more widths than it is given room for.
  std::array<std::uint16_t, 1> one = {1};
  measure_.Measure("abc", 0, ends, one);
  EXPECT_EQ(one[0], 4 * (10 + 10));
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

}  // namespace
