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

// A serif whose letters a to z move the pen 40 quarters and up, each a
// quarter more than the one before; an italic of it, each letter 10 more;
// and a fallback with a Greek alpha that neither has.
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

TEST_F(FaceMeasureTest, MeasuresAPieceAsItsGlyphsAdvancesAddedUp) {
  // "hyph" "en" "ation": h is 47, y 64, p 55, e 44, n 53, a 40, t 59, i 48, o 54.
  const std::array<std::uint8_t, 3> ends = {4, 6, 11};
  std::array<std::uint16_t, 3> widths{};
  measure_.Measure("hyphenation", 0, ends, widths);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 3>{47 + 64 + 55 + 47, 44 + 53, 40 + 59 + 48 + 54 + 53}));
  EXPECT_EQ(measure_.BytesMeasured(), 11U);

  // In another style, the other face's.
  EXPECT_EQ(Width("abc", 1), 50 + 51 + 52);
  EXPECT_EQ(measure_.BytesMeasured(), 14U);
  EXPECT_EQ(Width(""), 0);
}

TEST_F(FaceMeasureTest, MeasuresACharacterOfSeveralBytesInThePieceItBegins) {
  // "c", "é" and "s": the é is two bytes.
  const std::array<std::uint8_t, 2> ends = {3, 4};
  std::array<std::uint16_t, 2> widths{};
  measure_.Measure("c\xC3\xA9s", 0, ends, widths);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 2>{42 + 47, 58}));
}

TEST_F(FaceMeasureTest, GivesTheSpaceAndTheHyphenOfAStyle) {
  EXPECT_EQ(measure_.SpaceWidth(0), 20);
  EXPECT_EQ(measure_.SpaceWidth(1), 22);
  // The serif has U+2010 and is given it; the italic has a hyphen-minus only.
  EXPECT_EQ(measure_.HyphenWidth(0), 26);
  EXPECT_EQ(measure_.HyphenWidth(1), 28);
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
  // And they are measured as they are drawn: a, alpha, beta.
  EXPECT_EQ(Width("a\xCE\xB1\xCE\xB2"), 40 + 45 + 36);

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
            40 + 41 + 42);
}

TEST_F(FaceMeasureTest, MeasuresAByteThatIsNotUtf8AsTheReplacementCharacter) {
  // A byte that goes on with a character and begins none, a character cut
  // short, and one written the long way round: each byte is one.
  EXPECT_EQ(Width("a\x80"
                  "b"),
            40 + 60 + 41);
  EXPECT_EQ(Width("a\xC3"), 40 + 60);
  EXPECT_EQ(Width("\xC0\xAF"), 60 + 60);
  // The italic has no replacement character, nor the fallback: its glyph 0.
  EXPECT_EQ(Width("\xFF", 1), 38);
}

TEST_F(FaceMeasureTest, GivesAStyleWithNoFaceOfItsOwnStyleZeros) {
  EXPECT_EQ(measure_.FaceOf(2).Style(), 0);
  EXPECT_EQ(Width("abc", 2), 40 + 41 + 42);
  EXPECT_EQ(measure_.SpaceWidth(7), 20);
  // A style past the last there can be is style 0 as well.
  measure_.SetFace(FaceMeasure::kMostStyles, fonts_.Find(FaceRole::kSerif, bn::draw::kItalic, 20));
  EXPECT_EQ(Width("abc", static_cast<std::uint16_t>(FaceMeasure::kMostStyles)), 40 + 41 + 42);
  EXPECT_EQ(Width("abc", 0xFFFF), 40 + 41 + 42);
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

TEST_F(FaceMeasureTest, HoldsAPieceTooWideForItsNumberAtTheMost) {
  // 255 letters of 300 quarters each are more than 16 bits hold.
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
  EXPECT_EQ(one[0], 0xFFFF);
}

TEST_F(FaceMeasureTest, StopsAtTheEndOfTheTextWhateverThePiecesSay) {
  const std::array<std::uint8_t, 3> ends = {2, 200, 255};
  std::array<std::uint16_t, 3> widths = {1, 1, 1};
  measure_.Measure("abc", 0, ends, widths);
  EXPECT_EQ(widths, (std::array<std::uint16_t, 3>{40 + 41, 42, 0}));
  // And fills no more widths than it is given room for.
  std::array<std::uint16_t, 1> one = {1};
  measure_.Measure("abc", 0, ends, one);
  EXPECT_EQ(one[0], 40 + 41);
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
    // A line's natural width is its words' glyphs and the spaces between.
    const std::string_view words = text.substr(line.begin, line.end - line.begin);
    std::uint32_t wanted = 0;
    for (const char c : words) {
      wanted += c == ' ' ? 20U : 40U + static_cast<std::uint32_t>(c - 'a');
    }
    EXPECT_EQ(line.width, wanted) << words;
    EXPECT_LE(line.width, width) << words;
  }
}

}  // namespace
