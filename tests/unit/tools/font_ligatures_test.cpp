#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "font/ligatures.h"
#include "opentype_tables.h"

// The reader of a font's ligatures is given tables written here, a number
// at a time from the description of their layout (the OpenType table GSUB),
// so that each thing it has to know of them is tried by itself.

namespace {

using bn::font_tool::FaceLigature;
using bn::font_tool::FaceLigatures;
using bn::font_tool::FaceLigaturesOf;
using bn::font_tool::FontLigature;
using bn::font_tool::FontLigatures;
using bn::font_tool::LigatureGlyphs;
using bn::font_tool::ReadLigatures;
using bn::test::Append;
using bn::test::Coverage;
using bn::test::Lookup;
using bn::test::Put16;
using bn::test::Script;
using bn::test::Set16;
using Bytes = bn::test::TableBytes;

// One ligature of a set: the glyphs after the first, and the glyph that
// stands for them all.
struct Joined {
  std::vector<std::uint16_t> rest;
  std::uint16_t glyph = 0;
};

// A subtable of ligature substitutions: for each glyph a ligature begins
// with, in order, the ligatures that begin with it, in the order they are
// tried.
Bytes Ligatures(const std::map<std::uint16_t, std::vector<Joined>>& sets, bool coverageAsRanges = false) {
  std::vector<std::uint16_t> firsts;
  firsts.reserve(sets.size());
  for (const auto& [first, ligatures] : sets) {
    firsts.push_back(first);
  }
  Bytes out;
  Put16(out, 1);
  Put16(out, 0);  // Where the coverage is: set below.
  Put16(out, static_cast<std::uint32_t>(sets.size()));
  const std::size_t setOffsets = out.size();
  out.resize(out.size() + (2 * sets.size()), 0);
  std::size_t number = 0;
  for (const auto& [first, ligatures] : sets) {
    const std::size_t set = out.size();
    Set16(out, setOffsets + (2 * number), static_cast<std::uint32_t>(set));
    Put16(out, static_cast<std::uint32_t>(ligatures.size()));
    const std::size_t offsets = out.size();
    out.resize(out.size() + (2 * ligatures.size()), 0);
    for (std::size_t i = 0; i < ligatures.size(); ++i) {
      Set16(out, offsets + (2 * i), static_cast<std::uint32_t>(out.size() - set));
      Put16(out, ligatures[i].glyph);
      // How many glyphs it is made of, the first among them.
      Put16(out, static_cast<std::uint32_t>(ligatures[i].rest.size() + 1));
      for (const std::uint16_t glyph : ligatures[i].rest) {
        Put16(out, glyph);
      }
    }
    ++number;
  }
  Set16(out, 2, static_cast<std::uint32_t>(out.size()));
  Append(out, Coverage(firsts, coverageAsRanges));
  return out;
}

// A GSUB table of `scripts` and `lookups`: its extensions are of type 7.
Bytes Gsub(const std::vector<Script>& scripts, const std::vector<Lookup>& lookups) {
  return bn::test::LayoutTable(scripts, lookups, 7);
}

// A GSUB table whose one script makes the ligatures of all of `lookups`.
Bytes JoinedBy(const std::vector<Lookup>& lookups) {
  std::vector<std::uint16_t> all;
  all.reserve(lookups.size());
  for (std::size_t i = 0; i < lookups.size(); ++i) {
    all.push_back(static_cast<std::uint16_t>(i));
  }
  return Gsub({{.tag = "DFLT", .features = {{.tag = "liga", .lookups = all}}, .otherLanguage = {}}}, lookups);
}

// A lookup of ligature substitutions with these subtables.
Lookup Joining(std::vector<Bytes> subtables, bool extension = false) {
  return {.type = 4, .flag = 0, .subtables = std::move(subtables), .extension = extension};
}

using Found = std::vector<FontLigature>;
const std::vector<std::uint16_t> kGlyphs = {10, 11, 12, 13, 14};

TEST(FontLigatures, ReadsTheLigaturesAFontMakesInTheOrderItTriesThem) {
  // An f is 10, an i 11 and an l 12: ffi before fi before ff, as a font has
  // them, and a ligature of two other glyphs.
  const Bytes gsub = JoinedBy({Joining({Ligatures({
      {10, {{.rest = {10, 11}, .glyph = 50}, {.rest = {11}, .glyph = 51}, {.rest = {10}, .glyph = 52}}},
      {13, {{.rest = {14}, .glyph = 12}}},
  })})});
  const FontLigatures read = ReadLigatures(gsub, kGlyphs);
  EXPECT_EQ(read.ligatures, (Found{{{10, 10, 11}, 50}, {{10, 11}, 51}, {{10, 10}, 52}, {{13, 14}, 12}}));
  EXPECT_EQ(read.subtablesLeftOut, 0U);
  EXPECT_FALSE(read.damaged);

  // The same with the glyphs it covers written as ranges.
  const Bytes ranges = JoinedBy({Joining({Ligatures(
      {{10, {{.rest = {11}, .glyph = 51}}}, {11, {{.rest = {10}, .glyph = 53}}}, {13, {{.rest = {14}, .glyph = 12}}}},
      true)})});
  EXPECT_EQ(ReadLigatures(ranges, kGlyphs).ligatures, (Found{{{10, 11}, 51}, {{11, 10}, 53}, {{13, 14}, 12}}));
}

TEST(FontLigatures, ReadsALigatureOnlyIfEveryGlyphItIsMadeOfWasAskedAbout) {
  const Bytes gsub = JoinedBy({Joining({Ligatures({
      {10, {{.rest = {99}, .glyph = 50}, {.rest = {11, 99}, .glyph = 51}, {.rest = {11}, .glyph = 98}}},
      {97, {{.rest = {10}, .glyph = 52}}},
  })})});
  // The glyph that stands for a ligature may be one that was not asked
  // about: a font has glyphs for ligatures that no character has.
  EXPECT_EQ(ReadLigatures(gsub, kGlyphs).ligatures, (Found{{{10, 11}, 98}}));
  // Asked about more, it reads more.
  const std::vector<std::uint16_t> more = {10, 11, 97, 99};
  EXPECT_EQ(ReadLigatures(gsub, more).ligatures,
            (Found{{{10, 99}, 50}, {{10, 11, 99}, 51}, {{10, 11}, 98}, {{97, 10}, 52}}));
}

TEST(FontLigatures, GoesThroughALookupsSubtablesAndTheLookupsInOrder) {
  const Bytes gsub = JoinedBy({
      Joining({Ligatures({{10, {{.rest = {11}, .glyph = 51}}}}), Ligatures({{10, {{.rest = {12}, .glyph = 52}}}})}),
      Joining({Ligatures({{10, {{.rest = {11}, .glyph = 59}}},
                          {13, {{.rest = {14, 14}, .glyph = 50}, {.rest = {14}, .glyph = 53}}}})}),
  });
  // One made of the same glyphs as an earlier one would never be come to,
  // and is left out.
  const FontLigatures read = ReadLigatures(gsub, kGlyphs);
  EXPECT_EQ(read.ligatures, (Found{{{10, 11}, 51}, {{10, 12}, 52}, {{13, 14, 14}, 50}, {{13, 14}, 53}}));
  EXPECT_EQ(read.ligaturesLeftOut, 0U);
}

TEST(FontLigatures, LeavesOutOfALaterLookupWhatTheFontWouldMakeOtherwise) {
  // A font goes through a line with its first lookup and then with its
  // second. The first joins 10 with 11 as 50, and 12 with 13.
  const Bytes gsub = JoinedBy({
      Joining({Ligatures({{10, {{.rest = {11}, .glyph = 50}}}, {12, {{.rest = {13}, .glyph = 51}}}})}),
      Joining({Ligatures({
          // Made of a glyph the first makes, first or last: a ligature made
          // in two steps.
          {50, {{.rest = {14}, .glyph = 60}}},
          {14,
           {// One of the first's lies within these two, at the end of one
            // and in the middle of the other, so the font never makes them.
            {.rest = {10, 11}, .glyph = 61},
            {.rest = {12, 13, 14}, .glyph = 62},
            // These the font makes: one ends with a glyph the first's end
            // with, and one with a glyph one of the first's begins with,
            // which is none of it.
            {.rest = {13}, .glyph = 63},
            {.rest = {12}, .glyph = 64},
            {.rest = {50}, .glyph = 68}}},
          // One of the first's begins this one, and the font never makes it;
          // the next only begins as one of them does, and it makes that.
          {10, {{.rest = {11, 14}, .glyph = 69}, {.rest = {14}, .glyph = 65}}},
      })}),
      // And a third lookup is held to the second as well as the first: one
      // of the second's lies within its first ligature, and its last is made
      // of a glyph the second makes.
      Joining({Ligatures({{13, {{.rest = {14, 13}, .glyph = 66}, {.rest = {13}, .glyph = 67}}},
                          {63, {{.rest = {10}, .glyph = 70}}}})}),
  });
  const std::vector<std::uint16_t> glyphs = {10, 11, 12, 13, 14, 50, 63};
  const FontLigatures read = ReadLigatures(gsub, glyphs);
  EXPECT_EQ(read.ligatures,
            (Found{{{10, 11}, 50}, {{12, 13}, 51}, {{10, 14}, 65}, {{14, 13}, 63}, {{14, 12}, 64}, {{13, 13}, 67}}));
  EXPECT_EQ(read.ligaturesLeftOut, 7U);
  // Within one lookup nothing is at odds: it is tried at each place in
  // turn, as a list is. So a ligature another of the same lookup lies
  // within is made, and one made of a glyph the lookup makes.
  const Bytes one =
      JoinedBy({Joining({Ligatures({{10, {{.rest = {11}, .glyph = 50}}}}),
                         Ligatures({{12, {{.rest = {10, 11}, .glyph = 51}}}, {50, {{.rest = {10}, .glyph = 52}}}})})});
  const FontLigatures same = ReadLigatures(one, glyphs);
  EXPECT_EQ(same.ligatures, (Found{{{10, 11}, 50}, {{12, 10, 11}, 51}, {{50, 10}, 52}}));
  EXPECT_EQ(same.ligaturesLeftOut, 0U);
  // A font that joins an f and an i in one lookup and two f's and an i in
  // the next never makes the second of them: it has joined the f and the i
  // by then.
  const Bytes never = JoinedBy({Joining({Ligatures({{10, {{.rest = {11}, .glyph = 51}}}})}),
                                Joining({Ligatures({{10, {{.rest = {10, 11}, .glyph = 50}}}})})});
  EXPECT_EQ(ReadLigatures(never, glyphs).ligatures, (Found{{{10, 11}, 51}}));
  EXPECT_EQ(ReadLigatures(never, glyphs).ligaturesLeftOut, 1U);
  // One that joins two f's and an i, and an f and an i, in one lookup, and
  // two f's and an l, an f and an l, and two f's in the next, makes them
  // all: none of the first's lies within one of the second's.
  const Bytes split = JoinedBy({
      Joining({Ligatures({{10, {{.rest = {10, 11}, .glyph = 50}, {.rest = {11}, .glyph = 51}}}})}),
      Joining({Ligatures(
          {{10, {{.rest = {10, 12}, .glyph = 52}, {.rest = {12}, .glyph = 53}, {.rest = {10}, .glyph = 54}}}})}),
  });
  EXPECT_EQ(ReadLigatures(split, glyphs).ligatures,
            (Found{{{10, 10, 11}, 50}, {{10, 11}, 51}, {{10, 10, 12}, 52}, {{10, 12}, 53}, {{10, 10}, 54}}));
  EXPECT_EQ(ReadLigatures(split, glyphs).ligaturesLeftOut, 0U);
}

TEST(FontLigatures, ReadsTheLigaturesOfEveryScriptsDefaultLanguageAndNoOtherFeature) {
  const std::vector<Lookup> lookups = {
      Joining({Ligatures({{10, {{.rest = {11}, .glyph = 51}}}})}),
      Joining({Ligatures({{11, {{.rest = {12}, .glyph = 52}}}})}),
      Joining({Ligatures({{12, {{.rest = {13}, .glyph = 53}}}})}),
      Joining({Ligatures({{13, {{.rest = {14}, .glyph = 54}}}})}),
  };
  const Bytes gsub = Gsub(
      {
          {.tag = "DFLT",
           .features = {{.tag = "dlig", .lookups = {2}}, {.tag = "liga", .lookups = {1}}},
           .otherLanguage = {}},
          {.tag = "latn",
           .features = {{.tag = "liga", .lookups = {0, 1}}},
           .otherLanguage = {{.tag = "liga", .lookups = {3}}}},
          {.tag = "cyrl", .features = {}, .otherLanguage = {{.tag = "liga", .lookups = {3}}}},
      },
      lookups);
  const FontLigatures read = ReadLigatures(gsub, kGlyphs);
  // Lookup 0 and lookup 1, once, in the order of their numbers; and neither
  // the one a feature for ligatures at the reader's choice names, nor the
  // one only other languages name.
  EXPECT_EQ(read.ligatures, (Found{{{10, 11}, 51}, {{11, 12}, 52}}));
  EXPECT_FALSE(read.damaged);

  // A feature that names a lookup there is none of names nothing.
  const Bytes beyond =
      Gsub({{.tag = "DFLT", .features = {{.tag = "liga", .lookups = {0, 9}}}, .otherLanguage = {}}}, lookups);
  EXPECT_EQ(ReadLigatures(beyond, kGlyphs).ligatures, (Found{{{10, 11}, 51}}));
}

TEST(FontLigatures, ReadsALookupWrittenAsAnExtension) {
  const Bytes gsub = JoinedBy({Joining(
      {Ligatures({{10, {{.rest = {11}, .glyph = 51}}}}), Ligatures({{12, {{.rest = {13, 14}, .glyph = 52}}}})}, true)});
  const FontLigatures read = ReadLigatures(gsub, kGlyphs);
  EXPECT_EQ(read.ligatures, (Found{{{10, 11}, 51}, {{12, 13, 14}, 52}}));
  EXPECT_EQ(read.subtablesLeftOut, 0U);

  // A subtable need not be straight after the extension that says where it
  // is: here six bytes of something else come between the two.
  Bytes apart = JoinedBy({Joining({Ligatures({{10, {{.rest = {11}, .glyph = 51}}}})}, true)});
  const Bytes extension = {0, 1, 0, 4, 0, 0, 0, 8};
  const auto found = std::search(apart.begin(), apart.end(), extension.begin(), extension.end());
  ASSERT_TRUE(found != apart.end());
  const std::size_t place = static_cast<std::size_t>(found - apart.begin());
  apart[place + 7] = 14;
  apart.insert(apart.begin() + static_cast<std::ptrdiff_t>(place + 8), 6, std::uint8_t{0xFF});
  EXPECT_EQ(ReadLigatures(apart, kGlyphs).ligatures, (Found{{{10, 11}, 51}}));
  EXPECT_FALSE(ReadLigatures(apart, kGlyphs).damaged);
}

TEST(FontLigatures, LeavesOutWhatIsNotALigatureSubstitutionAndCountsIt) {
  // Subtables of another type are not read, whatever is in them here; nor
  // is one of a format there is none of.
  Bytes otherFormat = Ligatures({{10, {{.rest = {12}, .glyph = 58}}}});
  Set16(otherFormat, 0, 2);
  const Bytes gsub = JoinedBy({
      {.type = 6, .flag = 0, .subtables = {Ligatures({{10, {{.rest = {11}, .glyph = 59}}}})}, .extension = false},
      Joining({otherFormat, Ligatures({{10, {{.rest = {11}, .glyph = 51}}}})}),
      {.type = 1,
       .flag = 0,
       .subtables = {Ligatures({{10, {{.rest = {13}, .glyph = 57}}}}),
                     Ligatures({{10, {{.rest = {14}, .glyph = 56}}}})},
       .extension = true},
  });
  const FontLigatures read = ReadLigatures(gsub, kGlyphs);
  EXPECT_EQ(read.ligatures, (Found{{{10, 11}, 51}}));
  EXPECT_EQ(read.subtablesLeftOut, 4U);
}

TEST(FontLigatures, TakesAsALigatureOnlyWhatIsMadeOfTwoGlyphsOrMore) {
  // A ligature that says it is made of one glyph, which is another glyph
  // put for it and no ligature; and one that says none.
  Bytes table = Ligatures({{10, {{.rest = {}, .glyph = 50}, {.rest = {11}, .glyph = 51}}}});
  EXPECT_EQ(ReadLigatures(JoinedBy({Joining({table})}), kGlyphs).ligatures, (Found{{{10, 11}, 51}}));
  // The first ligature of the one set: after the subtable's six bytes and
  // one set's offset, the set's count and two offsets.
  const std::size_t first = 8 + 2 + 4;
  ASSERT_EQ(table[first + 1], 50);
  ASSERT_EQ(table[first + 3], 1);
  table[first + 3] = 0;
  EXPECT_EQ(ReadLigatures(JoinedBy({Joining({table})}), kGlyphs).ligatures, (Found{{{10, 11}, 51}}));
  // A list that covers more glyphs than it has sets for says nothing of
  // the others.
  Bytes fewer = Ligatures({{10, {{.rest = {11}, .glyph = 51}}}, {12, {{.rest = {13}, .glyph = 52}}}});
  ASSERT_EQ(fewer[5], 2);
  fewer[5] = 1;
  EXPECT_EQ(ReadLigatures(JoinedBy({Joining({fewer})}), kGlyphs).ligatures, (Found{{{10, 11}, 51}}));
}

TEST(FontLigatures, HasNoneWithoutATableOrWithoutGlyphs) {
  EXPECT_TRUE(ReadLigatures({}, kGlyphs).ligatures.empty());
  EXPECT_FALSE(ReadLigatures({}, kGlyphs).damaged);
  const Bytes gsub = JoinedBy({Joining({Ligatures({{10, {{.rest = {11}, .glyph = 51}}}})})});
  EXPECT_TRUE(ReadLigatures(gsub, {}).ligatures.empty());
  EXPECT_TRUE(ReadLigatures(Gsub({}, {}), kGlyphs).ligatures.empty());
}

// A table with a little of everything in it.
Bytes MixedGsub() {
  return Gsub(
      {
          {.tag = "DFLT", .features = {{.tag = "liga", .lookups = {0, 1}}}, .otherLanguage = {}},
          {.tag = "latn", .features = {{.tag = "liga", .lookups = {2}}}, .otherLanguage = {}},
      },
      {
          Joining({Ligatures({
              {10, {{.rest = {10, 11}, .glyph = 50}, {.rest = {11}, .glyph = 51}, {.rest = {10}, .glyph = 52}}},
              {13, {{.rest = {14}, .glyph = 56}}},
          })}),
          Joining({Ligatures({{11, {{.rest = {12, 12}, .glyph = 53}}}, {12, {{.rest = {12}, .glyph = 54}}}}, true)},
                  true),
          Joining({Ligatures({{14, {{.rest = {14}, .glyph = 55}}}})}),
      });
}

TEST(FontLigatures, ReadsNothingFromOutsideATableThatIsCutShort) {
  const Bytes good = MixedGsub();
  const FontLigatures whole = ReadLigatures(good, kGlyphs);
  ASSERT_FALSE(whole.damaged);
  ASSERT_EQ(whole.ligatures.size(), 7U);
  std::size_t damaged = 0;
  for (std::size_t length = 0; length < good.size(); ++length) {
    // In memory of its own size, so that a byte read past the cut is seen.
    const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    const FontLigatures read = ReadLigatures(cut, kGlyphs);
    damaged += read.damaged ? 1U : 0U;
    // Whatever it made of what was left, it is made of the glyphs asked
    // about, and is one of the whole table's.
    for (const FontLigature& ligature : read.ligatures) {
      ASSERT_TRUE(std::find(whole.ligatures.begin(), whole.ligatures.end(), ligature) != whole.ligatures.end())
          << length;
    }
  }
  // All but the shortest, which have not so much as a script, are seen to
  // be cut.
  EXPECT_GT(damaged, good.size() - 12);
}

TEST(FontLigatures, ReadsNothingFromOutsideATableWithAnyByteChanged) {
  const Bytes good = MixedGsub();
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      Bytes spoiled = good;
      spoiled[at] ^= change;
      const FontLigatures read = ReadLigatures(spoiled, kGlyphs);
      for (const FontLigature& ligature : read.ligatures) {
        ASSERT_GE(ligature.letters.size(), 2U) << at;
        for (const std::uint16_t letter : ligature.letters) {
          ASSERT_TRUE(std::find(kGlyphs.begin(), kGlyphs.end(), letter) != kGlyphs.end()) << at;
        }
      }
    }
  }
}

// ---- A font's ligatures as a face's ----

using Faces = std::vector<FaceLigature>;

TEST(FaceLigatures, GivesAFontsLigaturesByTheFacesOwnNumbersForItsGlyphs) {
  // The face's glyphs 1 to 5 are drawn from the font's 10, 11, 12, 51 and
  // 50: three letters and two glyphs that stand for ligatures, of which the
  // first is a character's own and the second no character's.
  const std::vector<std::uint32_t> drawnFrom = {0, 10, 11, 12, 51, 50};
  FontLigatures font;
  font.ligatures = {{{10, 10, 11}, 50}, {{10, 11}, 51}, {{12, 12}, 10}};
  const FaceLigatures face = FaceLigaturesOf(font, drawnFrom);
  EXPECT_EQ(face.ligatures, (Faces{{1, 1, 2, 5}, {1, 2, 0, 4}, {3, 3, 0, 1}}));
  EXPECT_EQ(face.leftOut, 0U);
  EXPECT_EQ(LigatureGlyphs(font), (std::vector<std::uint16_t>{50, 51, 10}));
}

TEST(FaceLigatures, LeavesOutWhatAFaceCannotHoldAndCountsIt) {
  const std::vector<std::uint32_t> drawnFrom = {0, 10, 11, 12, 51};
  FontLigatures font;
  font.ligatures = {
      {{10, 11, 12, 10}, 51},  // Of four glyphs.
      {{10, 11}, 60},          // Its glyph is not drawn in the face.
      {{10, 13}, 51},          // Nor is one it is made of,
      {{13, 10, 11}, 51},      //
      {{10, 11, 13}, 51},      // first, second or third.
      {{10}, 51},              // Of one glyph.
      {{11, 12}, 51},
  };
  const FaceLigatures face = FaceLigaturesOf(font, drawnFrom);
  EXPECT_EQ(face.ligatures, (Faces{{2, 3, 0, 4}}));
  EXPECT_EQ(face.leftOut, 6U);
  // With what was left out in reading the font, which no face has either.
  font.subtablesLeftOut = 20;
  font.ligaturesLeftOut = 300;
  EXPECT_EQ(FaceLigaturesOf(font, drawnFrom).leftOut, 326U);
  font.subtablesLeftOut = 0;
  font.ligaturesLeftOut = 0;
  // The glyphs to draw are those of ligatures of two glyphs or three, each
  // once: whether a face has what they are made of is the face's.
  EXPECT_EQ(LigatureGlyphs(font), (std::vector<std::uint16_t>{60, 51}));
  // A face's glyph 0, drawn for what it lacks, is in no ligature, whatever
  // the font draws it from.
  const std::vector<std::uint32_t> lacking = {11, 11, 12, 51};
  FontLigatures one;
  one.ligatures = {{{11, 12}, 51}};
  EXPECT_EQ(FaceLigaturesOf(one, lacking).ligatures, (Faces{{1, 2, 0, 3}}));
  EXPECT_TRUE(FaceLigaturesOf(one, std::vector<std::uint32_t>{}).ligatures.empty());
}

TEST(FaceLigatures, JoinsEveryGlyphOfAFaceThatIsDrawnFromAGlyphOfTheFont) {
  // The font draws two characters with its glyph 10 and two with its 11;
  // the glyph for the ligature is drawn twice too, and the first stands.
  const std::vector<std::uint32_t> drawnFrom = {0, 10, 11, 10, 50, 11, 50};
  FontLigatures font;
  font.ligatures = {{{10, 11}, 50}, {{11, 11, 10}, 50}};
  const FaceLigatures face = FaceLigaturesOf(font, drawnFrom);
  EXPECT_EQ(face.ligatures, (Faces{{1, 2, 0, 4},
                                   {1, 5, 0, 4},
                                   {3, 2, 0, 4},
                                   {3, 5, 0, 4},
                                   {2, 2, 1, 4},
                                   {2, 2, 3, 4},
                                   {2, 5, 1, 4},
                                   {2, 5, 3, 4},
                                   {5, 2, 1, 4},
                                   {5, 2, 3, 4},
                                   {5, 5, 1, 4},
                                   {5, 5, 3, 4}}));
  EXPECT_EQ(face.leftOut, 0U);
}

TEST(FaceLigatures, HoldsNoMoreThanAFaceHas) {
  // 300 ligatures, each of a glyph of its own with the next.
  std::vector<std::uint32_t> drawnFrom = {0};
  FontLigatures font;
  for (std::uint16_t glyph = 100; glyph < 401; ++glyph) {
    drawnFrom.push_back(glyph);
  }
  for (std::uint16_t glyph = 100; glyph < 400; ++glyph) {
    font.ligatures.push_back({{glyph, static_cast<std::uint16_t>(glyph + 1)}, 100});
  }
  const FaceLigatures face = FaceLigaturesOf(font, drawnFrom);
  ASSERT_EQ(face.ligatures.size(), 255U);
  EXPECT_EQ(face.leftOut, 45U);
  EXPECT_EQ(face.ligatures.front(), (FaceLigature{1, 2, 0, 1}));
  EXPECT_EQ(face.ligatures.back(), (FaceLigature{255, 256, 0, 1}));
}

}  // namespace
