#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "draw/font.h"
#include "font/build.h"
#include "font/preset.h"
#include "font/sizes.h"
#include "font_image.h"

// BN_COURIER_PRIME_DIR is the folder the build downloaded Courier Prime's
// font files to (third_party/courier-prime): the real font the builder is
// tried on.

namespace {

using bn::draw::Face;
using bn::draw::FaceRole;
using bn::draw::FontImage;
using bn::draw::FontStatus;
using bn::draw::Glyph;
using bn::draw::kBold;
using bn::draw::kItalic;
using bn::font_tool::BuildImage;
using bn::font_tool::Built;
using bn::font_tool::CodePoints;
using bn::font_tool::Preset;
using bn::font_tool::ReadPreset;

// ---- Presets ----

TEST(FontPreset, ReadsTheSizesTheFacesAndTheCharacters) {
  const Preset preset = ReadPreset(
      "sizes 26 30 34\n"
      "serif regular      /fonts/Serif-Regular.ttf\n"
      "serif bold-italic  /fonts/Serif-BoldItalic.ttf\n"
      "sans italic        /fonts/Sans-Italic.ttf\n"
      "mono bold          /fonts/Mono-Bold.ttf\n"
      "fallback regular   /fonts/Everything.otf\n"
      "characters U+0020-U+007E u+00a0 U+2010-U+2015\n",
      "/presets");
  ASSERT_TRUE(preset.ok) << preset.error;
  EXPECT_EQ(preset.sizes, (std::vector<std::uint16_t>{26, 30, 34}));
  ASSERT_EQ(preset.faces.size(), 5U);
  EXPECT_EQ(preset.faces[0].role, FaceRole::kSerif);
  EXPECT_EQ(preset.faces[0].style, 0);
  EXPECT_EQ(preset.faces[0].path, "/fonts/Serif-Regular.ttf");
  EXPECT_EQ(preset.faces[0].line, 2U);
  EXPECT_EQ(preset.faces[1].style, kBold | kItalic);
  EXPECT_EQ(preset.faces[2].role, FaceRole::kSans);
  EXPECT_EQ(preset.faces[2].style, kItalic);
  EXPECT_EQ(preset.faces[3].role, FaceRole::kMono);
  EXPECT_EQ(preset.faces[3].style, kBold);
  EXPECT_EQ(preset.faces[4].role, FaceRole::kFallback);
  EXPECT_EQ(preset.faces[4].line, 6U);
  EXPECT_EQ(preset.characters, (std::vector<CodePoints>{{0x20, 0x7E}, {0xA0, 0xA0}, {0x2010, 0x2015}}));
}

TEST(FontPreset, FindsAFileFromThePresetsFolderUnlessItIsNamedFromTheRoot) {
  const std::string_view text =
      "sizes 20\n"
      "serif regular  Fonts/My Serif Regular.ttf  \n"
      "serif bold     /Library/Fonts/Bold.ttf\n";
  const Preset preset = ReadPreset(text, "/home/someone/presets");
  ASSERT_TRUE(preset.ok) << preset.error;
  // The rest of the line is the file, spaces and all, but those at its end.
  EXPECT_EQ(preset.faces[0].path, "/home/someone/presets/Fonts/My Serif Regular.ttf");
  EXPECT_EQ(preset.faces[1].path, "/Library/Fonts/Bold.ttf");
  // With no folder, a file is as it was named.
  EXPECT_EQ(ReadPreset(text, "").faces[0].path, "Fonts/My Serif Regular.ttf");
}

TEST(FontPreset, PassesOverCommentsAndEmptyLinesAndPutsTheSizesInOrder) {
  const Preset preset = ReadPreset(
      "# A preset.\r\n"
      "\r\n"
      "   sizes 34 26   # The large one first.\r\n"
      "sizes 30 26\r\n"
      "\t\n"
      "mono regular Mono.ttf # The only face.\r\n",
      "/p");
  ASSERT_TRUE(preset.ok) << preset.error;
  EXPECT_EQ(preset.sizes, (std::vector<std::uint16_t>{26, 30, 34}));
  ASSERT_EQ(preset.faces.size(), 1U);
  EXPECT_EQ(preset.faces[0].path, "/p/Mono.ttf");
  EXPECT_EQ(preset.faces[0].line, 6U);
  // Naming no characters leaves the choice to the builder.
  EXPECT_TRUE(preset.characters.empty());
  EXPECT_FALSE(bn::font_tool::DefaultCharacters().empty());
}

TEST(FontPreset, SaysWhatIsWrongAndOnWhichLine) {
  const std::vector<std::pair<std::string_view, std::string_view>> wrong = {
      {"", "no sizes are given"},
      {"serif regular a.ttf\n", "no sizes are given"},
      {"sizes 20\n", "no faces are named"},
      {"sizes 20\n\nserif regular a.ttf\nsizes twenty\n",
       "line 4: a size is a number of pixels from 6 to 96, not 'twenty'"},
      {"sizes 5\n", "line 1: a size"},
      {"sizes 97\n", "line 1: a size"},
      {"sizes 20px\n", "line 1: a size"},
      {"sizes -20\n", "line 1: a size"},
      {"sizes 20\nserif regular a.ttf\nSerif regular b.ttf\n",
       "line 3: a line begins with sizes, characters, or a role"},
      {"sizes 20\nserif oblique a.ttf\n", "line 2: a style is regular, italic, bold or bold-italic, not 'oblique'"},
      {"sizes 20\nserif\n", "line 2: a style"},
      {"sizes 20\nserif bold\n", "line 2: no font file is named"},
      {"sizes 20\nserif bold # a.ttf\n", "line 2: no font file is named"},
      {"sizes 20\nserif bold a.ttf\nsans bold a.ttf\nserif bold b.ttf\n",
       "line 4: serif bold was named already, on line 2"},
      {"sizes 20\nmono regular a.ttf\ncharacters 0041\n",
       "line 3: characters are written U+0041 or U+0041-U+005A, not '0041'"},
      {"sizes 20\nmono regular a.ttf\ncharacters U+\n", "line 3: characters"},
      {"sizes 20\nmono regular a.ttf\ncharacters U+110000\n", "line 3: characters"},
      {"sizes 20\nmono regular a.ttf\ncharacters U+0041-\n", "line 3: characters"},
      {"sizes 20\nmono regular a.ttf\ncharacters U+005A-U+0041\n", "line 3: characters"},
      {"sizes 20\nmono regular a.ttf\ncharacters U+00G1\n", "line 3: characters"},
  };
  for (const auto& [text, error] : wrong) {
    const Preset preset = ReadPreset(text, "/p");
    EXPECT_FALSE(preset.ok) << text;
    EXPECT_EQ(preset.error.find(error), 0U) << text << "\nsaid: " << preset.error;
  }
}

TEST(FontPreset, NamesTheRolesAndStylesAsAPresetWritesThem) {
  EXPECT_EQ(bn::font_tool::RoleName(FaceRole::kSerif), "serif");
  EXPECT_EQ(bn::font_tool::RoleName(FaceRole::kSans), "sans");
  EXPECT_EQ(bn::font_tool::RoleName(FaceRole::kMono), "mono");
  EXPECT_EQ(bn::font_tool::RoleName(FaceRole::kFallback), "fallback");
  EXPECT_EQ(bn::font_tool::StyleName(0), "regular");
  EXPECT_EQ(bn::font_tool::StyleName(kBold), "bold");
  EXPECT_EQ(bn::font_tool::StyleName(kItalic), "italic");
  EXPECT_EQ(bn::font_tool::StyleName(kBold | kItalic), "bold-italic");
}

// ---- Presets in parts, and the repository's own ----

std::string FileText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.good()) << path;
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST(FontPreset, ReadsSeveralPresetsAsOne) {
  const std::vector<bn::font_tool::PresetText> parts = {
      {.name = "ladder.preset", .text = "sizes 30 22\n", .folder = "/fonts"},
      {.name = "faces.preset", .text = "serif regular a/Serif.ttf\nsans regular /abs/Sans.ttf\n", .folder = "/fonts"},
      {.name = "mono.preset",
       .text = "# The monospace face.\nmono bold Mono-Bold.ttf\nsizes 26\ncharacters U+0041\n",
       .folder = "/elsewhere"},
  };
  const Preset preset = bn::font_tool::ReadPresets(parts);
  ASSERT_TRUE(preset.ok) << preset.error;
  EXPECT_EQ(preset.sizes, (std::vector<std::uint16_t>{22, 26, 30}));
  ASSERT_EQ(preset.faces.size(), 3U);
  // Each part's files are found from its own folder.
  EXPECT_EQ(preset.faces[0].path, "/fonts/a/Serif.ttf");
  EXPECT_EQ(preset.faces[1].path, "/abs/Sans.ttf");
  EXPECT_EQ(preset.faces[2].path, "/elsewhere/Mono-Bold.ttf");
  EXPECT_EQ(preset.faces[2].line, 2U);
  EXPECT_EQ(preset.characters, (std::vector<CodePoints>{{0x41, 0x41}}));

  // What is wrong is said with the part it is in; and a face may be named
  // once among them all.
  std::vector<bn::font_tool::PresetText> wrong = parts;
  wrong[2].text = "mono bold Mono-Bold.ttf\nmono heavy Mono-Heavy.ttf\n";
  EXPECT_EQ(bn::font_tool::ReadPresets(wrong).error.rfind("mono.preset: line 2: a style is ", 0), 0U);
  wrong[2].text = "serif regular Other.ttf\n";
  EXPECT_EQ(bn::font_tool::ReadPresets(wrong).error, "mono.preset: line 1: serif regular was named already, on line 1");
  // Parts that say no sizes among them, or no faces, are not a preset.
  EXPECT_EQ(bn::font_tool::ReadPresets(std::span(parts).subspan(1, 1)).error, "no sizes are given");
  EXPECT_EQ(bn::font_tool::ReadPresets(std::span(parts).first(1)).error, "no faces are named");
  EXPECT_EQ(bn::font_tool::ReadPresets({}).error, "no sizes are given");
}

TEST(FontPreset, TheRepositorysPresetsAreEachAWholePresetWithEitherMonospaceFace) {
  const std::string folder = BN_FONT_PRESETS_DIR;
  for (const std::string family : {"noto", "plex", "source"}) {
    for (const std::string& mono : {std::string("courier-prime"), family}) {
      std::vector<bn::font_tool::PresetText> parts;
      for (const std::string& name : {std::string("ladder"), family, "mono-" + mono}) {
        std::string path = folder;
        path += '/';
        path += name;
        path += ".preset";
        parts.push_back({.name = name, .text = FileText(path), .folder = "/downloaded"});
      }
      const Preset preset = bn::font_tool::ReadPresets(parts);
      ASSERT_TRUE(preset.ok) << family << " with " << mono << ": " << preset.error;
      // The five steps of the ladder.
      EXPECT_EQ(preset.sizes, (std::vector<std::uint16_t>{22, 26, 30, 34, 38}));
      EXPECT_TRUE(preset.characters.empty());
      // A serif and a sans in four styles and a monospace face in two; and
      // a fallback face, but for Noto, which is what the others fall back
      // to.
      std::map<FaceRole, std::set<std::uint8_t>> styles;
      for (const bn::font_tool::FaceFile& face : preset.faces) {
        styles[face.role].insert(face.style);
        // Found from the folder the build downloads to, each family in its own.
        EXPECT_EQ(face.path.rfind("/downloaded/", 0), 0U) << face.path;
        const std::string in = face.path.substr(12, face.path.find('/', 12) - 12);
        const std::string wanted = face.role == FaceRole::kMono       ? mono
                                   : face.role == FaceRole::kFallback ? "noto"
                                                                      : family;
        EXPECT_EQ(in, wanted) << face.path;
        EXPECT_EQ(face.path.substr(face.path.size() - 4), ".ttf") << face.path;
      }
      EXPECT_EQ(styles[FaceRole::kSerif], (std::set<std::uint8_t>{0, 1, 2, 3}));
      EXPECT_EQ(styles[FaceRole::kSans], (std::set<std::uint8_t>{0, 1, 2, 3}));
      EXPECT_EQ(styles[FaceRole::kMono], (std::set<std::uint8_t>{0, kBold}));
      EXPECT_EQ(styles.count(FaceRole::kFallback), family == "noto" ? 0U : 1U);
      EXPECT_EQ(preset.faces.size(), family == "noto" ? 10U : 11U);
    }
  }
}

// ---- The image, from a real font ----

// A preset of Courier Prime's regular and bold as the monospace face, with
// `rest` after them.
Preset CourierPreset(const std::string& rest) {
  const Preset preset = ReadPreset(
      "mono regular CourierPrime-Regular.ttf\n"
      "mono bold    CourierPrime-Bold.ttf\n" +
          rest,
      BN_COURIER_PRIME_DIR);
  EXPECT_TRUE(preset.ok) << preset.error;
  return preset;
}

// How much ink a glyph's picture has.
std::uint32_t InkOf(const Glyph& glyph) {
  std::uint32_t ink = 0;
  for (std::uint32_t y = 0; y < glyph.height; ++y) {
    for (std::uint32_t x = 0; x < glyph.width; ++x) {
      ink += glyph.Ink(x, y);
    }
  }
  return ink;
}

TEST(FontBuild, DrawsEachFaceAtEachSizeIntoAnImageTheReaderOpens) {
  const Built built = BuildImage(CourierPreset("sizes 28 16\ncharacters U+0020-U+007E\n"));
  ASSERT_TRUE(built.ok) << built.error;
  EXPECT_TRUE(built.warnings.empty());
  FontImage fonts;
  ASSERT_EQ(fonts.Open(built.image), FontStatus::kOk);
  // The regular at both sizes, then the bold.
  ASSERT_EQ(fonts.Faces(), 4U);
  ASSERT_EQ(built.faces.size(), 4U);
  const std::vector<std::pair<std::uint8_t, std::uint16_t>> wanted = {{0, 16}, {0, 28}, {kBold, 16}, {kBold, 28}};
  for (std::size_t i = 0; i < 4; ++i) {
    const Face face = fonts.FaceAt(i);
    EXPECT_EQ(face.Role(), FaceRole::kMono);
    EXPECT_EQ(face.Style(), wanted[i].first);
    EXPECT_EQ(face.Size(), wanted[i].second);
    // Every character of ASCII that is drawn, and glyph 0.
    EXPECT_EQ(face.Glyphs(), 96U);
    EXPECT_EQ(built.faces[i].glyphs, 96U);
    EXPECT_EQ(built.faces[i].lacking, 0U);
    EXPECT_EQ(built.faces[i].size, face.Size());
    EXPECT_EQ(built.faces[i].ascent, face.Ascent());
    EXPECT_EQ(built.faces[i].descent, face.Descent());
    EXPECT_EQ(built.faces[i].lineHeight, face.LineHeight());
    EXPECT_EQ(built.faces[i].xHeight, face.XHeight());
    // A face's numbers are those of a typeface at that size.
    EXPECT_GT(face.Ascent(), face.Size() / 2);
    EXPECT_LE(face.Ascent(), face.Size() * 3 / 2);
    EXPECT_GT(face.Descent(), 0);
    EXPECT_LT(face.Descent(), face.Size());
    EXPECT_GE(face.LineHeight(), face.Size());
    EXPECT_GT(face.XHeight(), face.Size() / 4);
    EXPECT_LT(face.XHeight(), face.Ascent());
    // Its pictures are those of its characters and of glyph 0.
    Glyph glyph;
    ASSERT_TRUE(face.Missing(glyph));
    std::size_t pictures = glyph.picture.size();
    for (std::uint32_t codePoint = 0x20; codePoint <= 0x7E; ++codePoint) {
      ASSERT_TRUE(face.Find(codePoint, glyph)) << codePoint;
      pictures += glyph.picture.size();
    }
    EXPECT_EQ(built.faces[i].pictureBytes, pictures);
  }
  EXPECT_TRUE(fonts.Find(FaceRole::kMono, kBold, 28).Valid());
  EXPECT_FALSE(fonts.Find(FaceRole::kMono, kItalic, 28).Valid());
}

TEST(FontBuild, GivesEveryGlyphOfAMonospaceFaceTheSameAdvance) {
  const Built built = BuildImage(CourierPreset("sizes 20 40\ncharacters U+0020-U+007E\n"));
  ASSERT_TRUE(built.ok) << built.error;
  FontImage fonts;
  ASSERT_EQ(fonts.Open(built.image), FontStatus::kOk);
  std::uint16_t at20 = 0;
  for (const std::uint16_t size : {std::uint16_t{20}, std::uint16_t{40}}) {
    const Face face = fonts.Find(FaceRole::kMono, 0, size);
    ASSERT_TRUE(face.Valid());
    Glyph glyph;
    ASSERT_TRUE(face.Find('M', glyph));
    const std::uint16_t advance = glyph.advance;
    // About six tenths of the size, in quarters of a pixel.
    EXPECT_GT(advance, size * 2);
    EXPECT_LT(advance, size * 3);
    for (std::uint32_t codePoint = 0x20; codePoint <= 0x7E; ++codePoint) {
      ASSERT_TRUE(face.Find(codePoint, glyph));
      EXPECT_EQ(glyph.advance, advance) << codePoint;
    }
    if (size == 20) {
      at20 = advance;
    } else {
      // The advance is the outline's, so twice the size is twice as far.
      EXPECT_EQ(advance, at20 * 2);
    }
  }
}

TEST(FontBuild, DrawsALetterInInkWhereItStandsAndASpaceInNone) {
  const Built built = BuildImage(CourierPreset("sizes 30\ncharacters U+0020-U+007E\n"));
  ASSERT_TRUE(built.ok) << built.error;
  FontImage fonts;
  ASSERT_EQ(fonts.Open(built.image), FontStatus::kOk);
  const Face regular = fonts.Find(FaceRole::kMono, 0, 30);
  const Face bold = fonts.Find(FaceRole::kMono, kBold, 30);

  Glyph space;
  ASSERT_TRUE(regular.Find(' ', space));
  EXPECT_TRUE(space.picture.empty());
  EXPECT_GT(space.advance, 0);

  Glyph x;
  ASSERT_TRUE(regular.Find('x', x));
  // It stands on the baseline and is as tall as the face says an x is.
  EXPECT_EQ(x.top, regular.XHeight());
  EXPECT_LE(x.height - x.top, 1);
  EXPECT_GE(x.height, x.top);
  EXPECT_GT(InkOf(x), 0U);
  // Some pixel of it is in full ink, and it is not a block of it.
  bool full = false;
  for (std::uint32_t y = 0; y < x.height; ++y) {
    for (std::uint32_t at = 0; at < x.width; ++at) {
      full = full || x.Ink(at, y) == 3;
    }
  }
  EXPECT_TRUE(full);
  EXPECT_LT(InkOf(x), 3U * x.width * x.height / 2);

  // A p goes below the baseline, a d above the x; a full stop is small and
  // low.
  Glyph p;
  Glyph d;
  Glyph stop;
  ASSERT_TRUE(regular.Find('p', p));
  ASSERT_TRUE(regular.Find('d', d));
  ASSERT_TRUE(regular.Find('.', stop));
  EXPECT_GT(p.height - p.top, 3);
  EXPECT_GT(d.top, x.top + 3);
  EXPECT_LT(stop.top, x.top);
  EXPECT_LT(InkOf(stop), InkOf(x));

  // The bold has more ink than the regular for the same letter.
  Glyph boldX;
  ASSERT_TRUE(bold.Find('x', boldX));
  EXPECT_GT(InkOf(boldX), InkOf(x));

  // Glyph 0 is the font's own for what it lacks: something is drawn.
  Glyph missing;
  ASSERT_TRUE(regular.Missing(missing));
  EXPECT_GT(missing.advance, 0);
}

TEST(FontBuild, LeavesOutWhatTheFontHasNoGlyphFor) {
  // The private use area, where Courier Prime has nothing, among letters it
  // has; some given twice over.
  const Built built =
      BuildImage(CourierPreset("sizes 18\ncharacters U+0061-U+0063 U+E000-U+E00F\ncharacters U+0062-U+0064 U+0041\n"));
  ASSERT_TRUE(built.ok) << built.error;
  ASSERT_EQ(built.faces.size(), 2U);
  EXPECT_EQ(built.faces[0].glyphs, 6U);  // A, a to d, and glyph 0.
  EXPECT_EQ(built.faces[0].lacking, 16U);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(built.image), FontStatus::kOk);
  const Face face = fonts.FaceAt(0);
  Glyph glyph;
  for (const std::uint32_t has : {0x41U, 0x61U, 0x62U, 0x63U, 0x64U}) {
    EXPECT_TRUE(face.Find(has, glyph)) << has;
  }
  for (const std::uint32_t lacks : {0x42U, 0x60U, 0x65U, 0xE000U, 0xE00FU}) {
    EXPECT_FALSE(face.Find(lacks, glyph)) << lacks;
  }
}

TEST(FontBuild, MakesTheSameImageEveryTime) {
  const Preset preset = CourierPreset("sizes 17 23\n");
  const Built first = BuildImage(preset);
  const Built second = BuildImage(preset);
  ASSERT_TRUE(first.ok) << first.error;
  ASSERT_TRUE(second.ok);
  EXPECT_EQ(first.image, second.image);
  // With no characters named, the set the firmware is built to show: more
  // than ASCII, and less than all that was asked, for no font has it all.
  EXPECT_GT(first.faces[0].glyphs, 200U);
  EXPECT_GT(first.faces[0].lacking, 0U);
  // The image says how long it is.
  ASSERT_GE(first.image.size(), 16U);
  std::size_t said = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    said |= std::size_t{first.image[8 + i]} << (8 * i);
  }
  EXPECT_EQ(said, first.image.size());
}

TEST(FontBuild, WarnsOfTheStylesAReadingFaceIsNotGiven) {
  // Courier Prime standing in for a serif with a regular and a bold, and a
  // sans with an italic alone.
  const Preset preset = ReadPreset(
      "sizes 16\n"
      "characters U+0041-U+005A\n"
      "serif regular CourierPrime-Regular.ttf\n"
      "serif bold    CourierPrime-Bold.ttf\n"
      "mono regular  CourierPrime-Regular.ttf\n",
      BN_COURIER_PRIME_DIR);
  ASSERT_TRUE(preset.ok) << preset.error;
  const Built built = BuildImage(preset);
  ASSERT_TRUE(built.ok) << built.error;
  // The monospace face is not expected to have more than it has.
  ASSERT_EQ(built.warnings.size(), 2U);
  EXPECT_EQ(built.warnings[0].find("serif italic is not named"), 0U) << built.warnings[0];
  EXPECT_EQ(built.warnings[1].find("serif bold-italic is not named"), 0U) << built.warnings[1];
  // The serif's faces come before the monospace one's, whatever the order
  // they were named in.
  ASSERT_EQ(built.faces.size(), 3U);
  EXPECT_EQ(built.faces[0].role, FaceRole::kSerif);
  EXPECT_EQ(built.faces[1].style, kBold);
  EXPECT_EQ(built.faces[2].role, FaceRole::kMono);

  const Preset noRegular =
      ReadPreset("sizes 16\ncharacters U+0041\nmono bold CourierPrime-Bold.ttf\n", BN_COURIER_PRIME_DIR);
  const Built bold = BuildImage(noRegular);
  ASSERT_TRUE(bold.ok) << bold.error;
  ASSERT_EQ(bold.warnings.size(), 1U);
  EXPECT_EQ(bold.warnings[0], "no role has a regular face");
}

TEST(FontBuild, SaysWhichFileItCannotReadAsAFont) {
  const Preset missing =
      ReadPreset("sizes 16\nmono regular CourierPrime-Regular.ttf\nmono bold No-Such-Font.ttf\n", BN_COURIER_PRIME_DIR);
  ASSERT_TRUE(missing.ok);
  const Built built = BuildImage(missing);
  EXPECT_FALSE(built.ok);
  EXPECT_TRUE(built.image.empty());
  EXPECT_NE(built.error.find("No-Such-Font.ttf: FreeType cannot read it as a font (named on line 3)"),
            std::string::npos)
      << built.error;

  // A file that is there and is not a font: this one.
  const Preset notAFont = ReadPreset(std::string("sizes 16\nmono regular ") + __FILE__ + "\n", "");
  ASSERT_TRUE(notAFont.ok);
  const Built second = BuildImage(notAFont);
  EXPECT_FALSE(second.ok);
  EXPECT_NE(second.error.find("FreeType cannot read it as a font (named on line 2)"), std::string::npos)
      << second.error;
}

// ---- What an image's pictures would take ----

// A glyph of `ink`, a level a pixel, `width` across.
struct Drawn {
  std::vector<std::uint8_t> packed;
  Glyph glyph;
};
Drawn GlyphOf(const std::vector<std::uint8_t>& ink, std::uint8_t width) {
  Drawn drawn;
  drawn.packed = bn::test::PackedInk(ink);
  drawn.glyph.width = width;
  drawn.glyph.height = static_cast<std::uint8_t>(ink.size() / width);
  drawn.glyph.picture = drawn.packed;
  return drawn;
}

// The levels of the pixels that `runs` stand for, read as font/sizes.h
// says they are written.
std::vector<std::uint8_t> RunsUndone(const std::vector<std::uint8_t>& runs) {
  std::vector<std::uint8_t> ink;
  for (const std::uint8_t run : runs) {
    ink.insert(ink.end(), std::size_t{run & 0x3FU} + 1, static_cast<std::uint8_t>(run >> 6U));
  }
  return ink;
}

// What zlib makes of `deflated`, which has nothing round it, into `size`
// bytes.
std::vector<std::uint8_t> Inflated(const std::vector<std::uint8_t>& deflated, std::size_t size) {
  std::vector<std::uint8_t> out(size + 1);
  z_stream stream{};
  EXPECT_EQ(inflateInit2(&stream, -15), Z_OK);
  std::vector<std::uint8_t> in = deflated;
  stream.next_in = in.data();
  stream.avail_in = static_cast<uInt>(in.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(inflate(&stream, Z_FINISH), Z_STREAM_END);
  out.resize(stream.total_out);
  inflateEnd(&stream);
  return out;
}

TEST(FontSizes, WritesAPictureAsRunsOfPixelsOfOneLevel) {
  // Eighty pixels of full ink: a run of 64, the longest there is, and one
  // of 16.
  const Drawn block = GlyphOf(std::vector<std::uint8_t>(80, 3), 8);
  EXPECT_EQ(bn::font_tool::RunLength(block.glyph), (std::vector<std::uint8_t>{0xFF, 0xCF}));
  // Runs go on from one row to the next, and a pixel by itself is a run.
  const std::vector<std::uint8_t> ink = {0, 0, 0, 1, 1, 0, 3, 3, 3, 3, 3, 2};
  const Drawn mixed = GlyphOf(ink, 4);
  EXPECT_EQ(bn::font_tool::RunLength(mixed.glyph), (std::vector<std::uint8_t>{0x02, 0x41, 0x00, 0xC4, 0x80}));
  // A picture of no pixels is no runs.
  EXPECT_TRUE(bn::font_tool::RunLength(Glyph{}).empty());

  // Whatever the picture, its runs are its pixels: some made up, of every
  // shape, with runs of every length among them.
  std::uint32_t state = 12345;
  for (int trial = 0; trial < 300; ++trial) {
    const auto next = [&] {
      state = (state * 1664525U) + 1013904223U;
      return state >> 16U;
    };
    const auto width = static_cast<std::uint8_t>(1 + (next() % 40));
    const std::size_t height = 1 + (next() % 40);
    std::vector<std::uint8_t> made;
    while (made.size() < width * height) {
      made.insert(made.end(), 1 + (next() % (trial % 2 == 0 ? 3 : 200)), static_cast<std::uint8_t>(next() % 4));
    }
    made.resize(width * height);
    const Drawn drawn = GlyphOf(made, width);
    const std::vector<std::uint8_t> runs = bn::font_tool::RunLength(drawn.glyph);
    ASSERT_EQ(RunsUndone(runs), made) << trial;
    // And no two runs next to each other could have been one.
    for (std::size_t i = 1; i < runs.size(); ++i) {
      ASSERT_TRUE((runs[i] >> 6U) != (runs[i - 1] >> 6U) || (runs[i - 1] & 0x3FU) == 63) << trial;
    }
  }
}

TEST(FontSizes, CompressesBytesSoThatZlibGivesThemBack) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(5000);
  for (int i = 0; i < 5000; ++i) {
    bytes.push_back(static_cast<std::uint8_t>((i % 7) * (i % 13)));
  }
  const std::vector<std::uint8_t> deflated = bn::font_tool::Deflated(bytes);
  EXPECT_LT(deflated.size(), bytes.size() / 4);
  EXPECT_EQ(Inflated(deflated, bytes.size()), bytes);
  // A byte, and none.
  EXPECT_EQ(Inflated(bn::font_tool::Deflated(std::vector<std::uint8_t>{0x5A}), 1), (std::vector<std::uint8_t>{0x5A}));
  EXPECT_TRUE(Inflated(bn::font_tool::Deflated({}), 0).empty());
}

TEST(FontSizes, AddsUpWhatEachWayOfKeepingThePicturesTakes) {
  // Two sizes. At 20, a face of 300 glyphs, each twelve by twelve and all
  // alike but for a pixel, and one of two glyphs; at 30, one of 40 glyphs.
  bn::test::ImageFace many = bn::test::EmptyFace(0, 0, 20);
  for (std::uint32_t i = 0; i < 300; ++i) {
    bn::test::ImageGlyph glyph = bn::test::Block(40, 12, 12, 3);
    glyph.ink[i % 144] = 1;
    many.glyphs[0x100 + i] = glyph;
  }
  bn::test::ImageFace few = bn::test::EmptyFace(1, 0, 20);
  few.glyphs['a'] = bn::test::Block(40, 9, 9, 2);
  few.glyphs[' '] = bn::test::Blank(20);
  bn::test::ImageFace large = bn::test::EmptyFace(0, 0, 30);
  for (std::uint32_t i = 0; i < 40; ++i) {
    large.glyphs['A' + i] =
        bn::test::Block(60, static_cast<std::uint8_t>(10 + (i % 5)), 15, static_cast<std::uint8_t>(1 + (i % 3)));
  }
  const std::vector<std::uint8_t> image = bn::test::FontImageBytes({large, many, few});
  const bn::font_tool::ImageSizes sizes = bn::font_tool::MeasureImage(image);
  ASSERT_TRUE(sizes.ok) << sizes.error;
  EXPECT_EQ(sizes.imageBytes, image.size());

  // The faces of each size together, in order of size; glyph 0 of each
  // face, eight by twelve, is counted with the rest.
  ASSERT_EQ(sizes.sizes.size(), 2U);
  EXPECT_EQ(sizes.sizes[0].size, 20);
  EXPECT_EQ(sizes.sizes[0].faces, 2U);
  EXPECT_EQ(sizes.sizes[0].glyphs, 301U + 3U);
  EXPECT_EQ(sizes.sizes[0].pictures.asTheyAre, (300U * 36U) + 24U + 21U + 24U);
  EXPECT_EQ(sizes.sizes[1].size, 30);
  EXPECT_EQ(sizes.sizes[1].faces, 1U);
  EXPECT_EQ(sizes.sizes[1].glyphs, 41U);
  EXPECT_EQ(sizes.all.faces, 3U);
  EXPECT_EQ(sizes.all.glyphs, 345U);
  EXPECT_EQ(sizes.all.pictures.asTheyAre, sizes.sizes[0].pictures.asTheyAre + sizes.sizes[1].pictures.asTheyAre);
  EXPECT_EQ(sizes.tableBytes, image.size() - sizes.all.pictures.asTheyAre);

  // Each way is what its own function makes of each glyph, added up.
  bn::draw::FontImage fonts;
  ASSERT_EQ(fonts.Open(image), bn::draw::FontStatus::kOk);
  std::size_t runs = 0;
  std::size_t deflated = 0;
  std::size_t best = 0;
  for (std::size_t face = 0; face < fonts.Faces(); ++face) {
    for (std::uint32_t index = 0; index < fonts.FaceAt(face).Glyphs(); ++index) {
      Glyph glyph;
      ASSERT_TRUE(fonts.FaceAt(face).GlyphAt(index, glyph));
      const std::size_t asRuns = bn::font_tool::RunLength(glyph).size();
      const std::size_t byDeflate = glyph.picture.empty() ? 0 : bn::font_tool::Deflated(glyph.picture).size();
      runs += asRuns;
      deflated += byDeflate;
      best += std::min({glyph.picture.size(), asRuns, byDeflate});
    }
  }
  const bn::font_tool::PictureSizes& all = sizes.all.pictures;
  EXPECT_EQ(all.runLength, runs);
  EXPECT_EQ(all.deflate, deflated);
  EXPECT_EQ(all.best, best);
  EXPECT_EQ(all.bestAsTheyAre + all.bestRunLength + all.bestDeflate, 345U);
  // Blocks of one level are a few runs each: the best way for them.
  EXPECT_GT(all.bestRunLength, 300U);
  EXPECT_LT(all.runLength, all.asTheyAre / 4);
  EXPECT_LE(all.best, all.runLength);

  // Glyphs alike compress far better together than apart, and the more
  // together the better.
  const bn::font_tool::PictureSizes& alike = sizes.sizes[0].pictures;
  EXPECT_LT(alike.groupsOf16, alike.deflate / 2);
  EXPECT_LT(alike.groupsOf64, alike.groupsOf16);
  EXPECT_LT(alike.groupsOf256, alike.groupsOf64);
  EXPECT_LT(alike.wholeFaces, alike.groupsOf256);
  // A face of fewer glyphs than a group is one group, whatever the group.
  const bn::font_tool::PictureSizes& larger = sizes.sizes[1].pictures;
  EXPECT_EQ(larger.groupsOf64, larger.wholeFaces);
  EXPECT_EQ(larger.groupsOf256, larger.wholeFaces);
  EXPECT_GT(larger.groupsOf16, larger.wholeFaces);

  // What is not an image is not measured.
  const bn::font_tool::ImageSizes none = bn::font_tool::MeasureImage(std::span(image).first(image.size() - 1));
  EXPECT_FALSE(none.ok);
  EXPECT_EQ(none.error, "bad image");
}

TEST(FontSizes, MeasuresTheImageOfARealFont) {
  const Built built = BuildImage(CourierPreset("sizes 30 18\ncharacters U+0020-U+007E\n"));
  ASSERT_TRUE(built.ok) << built.error;
  const bn::font_tool::ImageSizes sizes = bn::font_tool::MeasureImage(built.image);
  ASSERT_TRUE(sizes.ok) << sizes.error;
  ASSERT_EQ(sizes.sizes.size(), 2U);
  EXPECT_EQ(sizes.sizes[0].size, 18);
  EXPECT_EQ(sizes.sizes[1].size, 30);
  // What the builder said went into each face.
  EXPECT_EQ(sizes.sizes[0].pictures.asTheyAre, built.faces[0].pictureBytes + built.faces[2].pictureBytes);
  EXPECT_EQ(sizes.sizes[1].pictures.asTheyAre, built.faces[1].pictureBytes + built.faces[3].pictureBytes);
  EXPECT_EQ(sizes.all.glyphs, 4U * 96U);
  // Letters drawn in grays compress, and better together.
  const bn::font_tool::PictureSizes& all = sizes.all.pictures;
  EXPECT_LT(all.best, all.asTheyAre);
  EXPECT_LT(all.groupsOf16, all.best);
  EXPECT_LT(all.wholeFaces, all.groupsOf16);
  EXPECT_GT(all.wholeFaces, all.asTheyAre / 10);
}

}  // namespace
