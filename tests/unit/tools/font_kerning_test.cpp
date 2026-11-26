#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "font/kerning.h"

// The reader of a font's kerning is given tables written here, a number at
// a time from the description of their layout (the OpenType tables GPOS and
// GDEF), so that each thing it has to know of them is tried by itself.

namespace {

using bn::font_tool::FacePair;
using bn::font_tool::FontKerning;
using bn::font_tool::KernedPair;
using bn::font_tool::KerningClasses;
using bn::font_tool::MakeKerningClasses;
using bn::font_tool::ReadKerning;

using Bytes = std::vector<std::uint8_t>;

void Put16(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}
void Put32(Bytes& out, std::uint32_t value) {
  Put16(out, value >> 16U);
  Put16(out, value & 0xFFFFU);
}
void Set16(Bytes& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<std::uint8_t>(value >> 8U);
  out[at + 1] = static_cast<std::uint8_t>(value);
}
void Append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

// A coverage table of `glyphs`, which are in order: as a list, or as ranges
// of glyphs that follow one another.
Bytes Coverage(const std::vector<std::uint16_t>& glyphs, bool asRanges = false) {
  Bytes out;
  if (!asRanges) {
    Put16(out, 1);
    Put16(out, static_cast<std::uint32_t>(glyphs.size()));
    for (const std::uint16_t glyph : glyphs) {
      Put16(out, glyph);
    }
    return out;
  }
  struct Range {
    std::uint16_t first;
    std::uint16_t last;
    std::size_t place;
  };
  std::vector<Range> ranges;
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    if (!ranges.empty() && ranges.back().last + 1 == glyphs[i]) {
      ranges.back().last = glyphs[i];
    } else {
      ranges.push_back({glyphs[i], glyphs[i], i});
    }
  }
  Put16(out, 2);
  Put16(out, static_cast<std::uint32_t>(ranges.size()));
  for (const Range& range : ranges) {
    Put16(out, range.first);
    Put16(out, range.last);
    Put16(out, static_cast<std::uint32_t>(range.place));
  }
  return out;
}

// A table of classes: as a range for each glyph, or as a list of classes
// from the first glyph named to the last, with 0 for those between.
Bytes Classes(const std::map<std::uint16_t, std::uint16_t>& classOf, bool asList = false) {
  Bytes out;
  if (asList && !classOf.empty()) {
    const std::uint16_t first = classOf.begin()->first;
    const std::uint16_t last = classOf.rbegin()->first;
    Put16(out, 1);
    Put16(out, first);
    Put16(out, static_cast<std::uint32_t>(last - first + 1));
    for (std::uint32_t glyph = first; glyph <= last; ++glyph) {
      const auto found = classOf.find(static_cast<std::uint16_t>(glyph));
      Put16(out, found == classOf.end() ? 0U : found->second);
    }
    return out;
  }
  Put16(out, 2);
  Put16(out, static_cast<std::uint32_t>(classOf.size()));
  for (const auto& [glyph, value] : classOf) {
    Put16(out, glyph);
    Put16(out, glyph);
    Put16(out, value);
  }
  return out;
}

// What a value record may hold, of which only as much is written as the
// record's format has.
struct Value {
  int xPlacement = 0;
  int yPlacement = 0;
  int xAdvance = 0;
  int yAdvance = 0;
};
constexpr unsigned kXPlacement = 0x0001;
constexpr unsigned kYPlacement = 0x0002;
constexpr unsigned kXAdvance = 0x0004;
constexpr unsigned kYAdvance = 0x0008;

void PutValue(Bytes& out, std::uint16_t format, const Value& value) {
  if ((format & kXPlacement) != 0) {
    Put16(out, static_cast<std::uint16_t>(value.xPlacement));
  }
  if ((format & kYPlacement) != 0) {
    Put16(out, static_cast<std::uint16_t>(value.yPlacement));
  }
  if ((format & kXAdvance) != 0) {
    Put16(out, static_cast<std::uint16_t>(value.xAdvance));
  }
  if ((format & kYAdvance) != 0) {
    Put16(out, static_cast<std::uint16_t>(value.yAdvance));
  }
}

// One pair of a list of pairs: the second glyph, and the two glyphs' values.
struct Second {
  std::uint16_t glyph = 0;
  Value first;
  Value second;
};
// A list of pairs with the first glyph's advance alone.
Second After(std::uint16_t glyph, int advance) {
  return {.glyph = glyph, .first = {.xAdvance = advance}, .second = {}};
}

// A subtable of pair adjustments that is a list of pairs: for each first
// glyph, its seconds.
Bytes ListOfPairs(const std::map<std::uint16_t, std::vector<Second>>& pairs, std::uint16_t format1 = kXAdvance,
                  std::uint16_t format2 = 0, bool coverageAsRanges = false) {
  std::vector<std::uint16_t> firsts;
  firsts.reserve(pairs.size());
  for (const auto& [first, seconds] : pairs) {
    firsts.push_back(first);
  }
  Bytes out;
  Put16(out, 1);
  Put16(out, 0);  // Where the coverage is: set below.
  Put16(out, format1);
  Put16(out, format2);
  Put16(out, static_cast<std::uint32_t>(pairs.size()));
  const std::size_t offsets = out.size();
  out.resize(out.size() + (2 * pairs.size()), 0);
  std::size_t number = 0;
  for (const auto& [first, seconds] : pairs) {
    Set16(out, offsets + (2 * number), static_cast<std::uint32_t>(out.size()));
    Put16(out, static_cast<std::uint32_t>(seconds.size()));
    for (const Second& second : seconds) {
      Put16(out, second.glyph);
      PutValue(out, format1, second.first);
      PutValue(out, format2, second.second);
    }
    ++number;
  }
  Set16(out, 2, static_cast<std::uint32_t>(out.size()));
  Append(out, Coverage(firsts, coverageAsRanges));
  return out;
}

// A subtable of pair adjustments that is a table of classes: the glyphs it
// covers, the classes of first glyphs and of second, and for each class of
// the one and each of the other, the first glyph's advance.
Bytes TableOfClasses(const std::vector<std::uint16_t>& covered, const std::map<std::uint16_t, std::uint16_t>& class1,
                     const std::map<std::uint16_t, std::uint16_t>& class2,
                     const std::vector<std::vector<int>>& advances, bool classesAsLists = false) {
  Bytes out;
  Put16(out, 2);
  Put16(out, 0);  // The coverage,
  Put16(out, kXAdvance);
  Put16(out, 0);
  Put16(out, 0);  // and the two tables of classes: set below.
  Put16(out, 0);
  Put16(out, static_cast<std::uint32_t>(advances.size()));
  Put16(out, static_cast<std::uint32_t>(advances.empty() ? 0 : advances[0].size()));
  for (const std::vector<int>& row : advances) {
    for (const int advance : row) {
      Put16(out, static_cast<std::uint16_t>(advance));
    }
  }
  Set16(out, 2, static_cast<std::uint32_t>(out.size()));
  Append(out, Coverage(covered));
  Set16(out, 8, static_cast<std::uint32_t>(out.size()));
  Append(out, Classes(class1, classesAsLists));
  Set16(out, 10, static_cast<std::uint32_t>(out.size()));
  Append(out, Classes(class2, classesAsLists));
  return out;
}

// A lookup: its type, its flag, and its subtables. One written as an
// extension has type 9 and says where each subtable of `type` is.
struct Lookup {
  std::uint16_t type = 2;
  std::uint16_t flag = 0;
  std::vector<Bytes> subtables;
  bool extension = false;
};

// A script and the features its default language has: each a tag and the
// lookups it names. A script with no features has no default language. It
// may have one other language besides, with features of its own: none,
// for a script that has not.
struct Feature {
  std::string tag;
  std::vector<std::uint16_t> lookups;
};
struct Script {
  std::string tag;
  std::vector<Feature> features;
  std::vector<Feature> otherLanguage;
};

void PutTag(Bytes& out, const std::string& tag) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(i < tag.size() ? tag[i] : ' '));
  }
}

// A GPOS table of `scripts` and `lookups`.
Bytes Gpos(const std::vector<Script>& scripts, const std::vector<Lookup>& lookups) {
  // Every feature of every script, in order: a script's are numbered from
  // where the ones before it end.
  std::vector<Feature> features;
  for (const Script& script : scripts) {
    features.insert(features.end(), script.features.begin(), script.features.end());
    features.insert(features.end(), script.otherLanguage.begin(), script.otherLanguage.end());
  }
  Bytes out;
  Put32(out, 0x00010000U);
  Put16(out, 10);  // The scripts follow the header.
  Put16(out, 0);   // The features and the lookups: set below.
  Put16(out, 0);

  // The scripts.
  const std::size_t scriptList = out.size();
  Put16(out, static_cast<std::uint32_t>(scripts.size()));
  const std::size_t scriptRecords = out.size();
  out.resize(out.size() + (6 * scripts.size()), 0);
  std::size_t feature = 0;
  for (std::size_t s = 0; s < scripts.size(); ++s) {
    Bytes record;
    PutTag(record, scripts[s].tag);
    Put16(record, static_cast<std::uint32_t>(out.size() - scriptList));
    std::copy(record.begin(), record.end(), out.begin() + static_cast<std::ptrdiff_t>(scriptRecords + (6 * s)));
    // Where its default language is, or that it has none; then its other
    // language, if it has one; then the languages themselves.
    const std::size_t script = out.size();
    const bool other = !scripts[s].otherLanguage.empty();
    Put16(out, 0);
    Put16(out, other ? 1U : 0U);
    const std::size_t otherRecord = out.size();
    if (other) {
      PutTag(out, "TRK ");
      Put16(out, 0);
    }
    const auto language = [&](const std::vector<Feature>& its) {
      Put16(out, 0);
      Put16(out, 0xFFFF);  // No feature it must have.
      Put16(out, static_cast<std::uint32_t>(its.size()));
      for (std::size_t f = 0; f < its.size(); ++f) {
        Put16(out, static_cast<std::uint32_t>(feature++));
      }
    };
    if (!scripts[s].features.empty()) {
      Set16(out, script, static_cast<std::uint32_t>(out.size() - script));
      language(scripts[s].features);
    }
    if (other) {
      Set16(out, otherRecord + 4, static_cast<std::uint32_t>(out.size() - script));
      language(scripts[s].otherLanguage);
    }
  }

  // The features.
  const std::size_t featureList = out.size();
  Set16(out, 6, static_cast<std::uint32_t>(featureList));
  Put16(out, static_cast<std::uint32_t>(features.size()));
  const std::size_t featureRecords = out.size();
  out.resize(out.size() + (6 * features.size()), 0);
  for (std::size_t f = 0; f < features.size(); ++f) {
    Bytes record;
    PutTag(record, features[f].tag);
    Put16(record, static_cast<std::uint32_t>(out.size() - featureList));
    std::copy(record.begin(), record.end(), out.begin() + static_cast<std::ptrdiff_t>(featureRecords + (6 * f)));
    Put16(out, 0);
    Put16(out, static_cast<std::uint32_t>(features[f].lookups.size()));
    for (const std::uint16_t lookup : features[f].lookups) {
      Put16(out, lookup);
    }
  }

  // The lookups.
  const std::size_t lookupList = out.size();
  Set16(out, 8, static_cast<std::uint32_t>(lookupList));
  Put16(out, static_cast<std::uint32_t>(lookups.size()));
  const std::size_t lookupOffsets = out.size();
  out.resize(out.size() + (2 * lookups.size()), 0);
  for (std::size_t l = 0; l < lookups.size(); ++l) {
    const Lookup& lookup = lookups[l];
    const std::size_t at = out.size();
    Set16(out, lookupOffsets + (2 * l), static_cast<std::uint32_t>(at - lookupList));
    Put16(out, lookup.extension ? 9U : lookup.type);
    Put16(out, lookup.flag);
    Put16(out, static_cast<std::uint32_t>(lookup.subtables.size()));
    const std::size_t offsets = out.size();
    out.resize(out.size() + (2 * lookup.subtables.size()), 0);
    for (std::size_t i = 0; i < lookup.subtables.size(); ++i) {
      Set16(out, offsets + (2 * i), static_cast<std::uint32_t>(out.size() - at));
      if (lookup.extension) {
        Put16(out, 1);
        Put16(out, lookup.type);
        Put32(out, 8);  // The subtable is straight after these eight bytes.
      }
      Append(out, lookup.subtables[i]);
    }
  }
  return out;
}

// A GPOS table whose one script kerns by all of `lookups`.
Bytes KernedBy(const std::vector<Lookup>& lookups) {
  std::vector<std::uint16_t> all;
  all.reserve(lookups.size());
  for (std::size_t i = 0; i < lookups.size(); ++i) {
    all.push_back(static_cast<std::uint16_t>(i));
  }
  return Gpos({{.tag = "DFLT", .features = {{.tag = "kern", .lookups = all}}, .otherLanguage = {}}}, lookups);
}

// A GDEF table that puts glyphs in classes: 1 a base glyph, 2 a ligature,
// 3 a mark.
Bytes Gdef(const std::map<std::uint16_t, std::uint16_t>& classOf) {
  Bytes out;
  Put32(out, 0x00010000U);
  Put16(out, 12);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 0);
  Append(out, Classes(classOf));
  return out;
}

using Pairs = std::vector<KernedPair>;
const std::vector<std::uint16_t> kGlyphs = {10, 11, 12, 13, 20, 21, 22, 30};

TEST(FontKerning, ReadsAListOfPairs) {
  const Bytes gpos = KernedBy({{.subtables = {ListOfPairs({
                                    {10, {After(20, -50), After(21, 30), After(99, -5)}},
                                    {11, {After(20, -10)}},
                                    {98, {After(20, -70)}},
                                })}}});
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  // In order of the first glyph and then the second; and nothing of a glyph
  // that was not asked about, whichever of the two it is.
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -50}, {10, 21, 30}, {11, 20, -10}}));
  EXPECT_EQ(kerning.lookupsLeftOut, 0U);
  EXPECT_EQ(kerning.otherAdjustments, 0U);
  EXPECT_FALSE(kerning.damaged);

  // The same with the glyphs it covers written as ranges.
  const Bytes ranges =
      KernedBy({{.subtables = {ListOfPairs({{10, {After(20, -50)}}, {11, {After(20, -10)}}, {13, {After(21, 4)}}},
                                           kXAdvance, 0, true)}}});
  EXPECT_EQ(ReadKerning(ranges, {}, kGlyphs).pairs, (Pairs{{10, 20, -50}, {11, 20, -10}, {13, 21, 4}}));
}

TEST(FontKerning, ReadsATableOfClasses) {
  // 10 and 11 kern alike, 12 otherwise, and 13 is covered and of no class;
  // after them 20 is of one class, 21 of another, and 22 of none.
  for (const bool asLists : {false, true}) {
    const Bytes gpos =
        KernedBy({{.subtables = {TableOfClasses({10, 11, 12, 13}, {{10, 1}, {11, 1}, {12, 2}}, {{20, 1}, {21, 2}},
                                                {
                                                    {0, 7, 0},     // No class, as 13 is.
                                                    {0, -40, 15},  // Class 1.
                                                    {-3, 0, -25},  // Class 2.
                                                },
                                                asLists)}}});
    const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
    EXPECT_EQ(kerning.pairs, (Pairs{
                                 {10, 20, -40},
                                 {10, 21, 15},
                                 {11, 20, -40},
                                 {11, 21, 15},
                                 // 12 kerns with every glyph of no class as a second: itself and the rest.
                                 {12, 10, -3},
                                 {12, 11, -3},
                                 {12, 12, -3},
                                 {12, 13, -3},
                                 {12, 21, -25},
                                 {12, 22, -3},
                                 {12, 30, -3},
                                 {13, 20, 7},
                             }))
        << asLists;
    EXPECT_FALSE(kerning.damaged);
  }
}

TEST(FontKerning, TriesALookupsSubtablesInOrder) {
  const Bytes list = ListOfPairs({{10, {After(20, -50), After(21, 0)}}});
  const Bytes table =
      TableOfClasses({10, 11}, {{10, 1}, {11, 1}}, {{20, 1}, {21, 1}, {22, 2}}, {{0, 0, 0}, {0, -9, -8}});
  // The list first: what it names is as it says, nothing too, and what it
  // does not name goes on to the table.
  EXPECT_EQ(ReadKerning(KernedBy({{.subtables = {list, table}}}), {}, kGlyphs).pairs,
            (Pairs{{10, 20, -50}, {10, 22, -8}, {11, 20, -9}, {11, 21, -9}, {11, 22, -8}}));
  // The table first: it answers for every pair whose first glyph it covers,
  // and the list is never come to for them.
  EXPECT_EQ(ReadKerning(KernedBy({{.subtables = {table, list}}}), {}, kGlyphs).pairs,
            (Pairs{{10, 20, -9}, {10, 21, -9}, {10, 22, -8}, {11, 20, -9}, {11, 21, -9}, {11, 22, -8}}));
  // A table that does not cover the first glyph says nothing of it.
  const Bytes other = TableOfClasses({11}, {{11, 1}}, {{20, 1}}, {{0, 0}, {0, -9}});
  EXPECT_EQ(ReadKerning(KernedBy({{.subtables = {other, list}}}), {}, kGlyphs).pairs,
            (Pairs{{10, 20, -50}, {11, 20, -9}}));
}

TEST(FontKerning, AddsUpWhatSeveralLookupsSayOfAPair) {
  const Bytes gpos = KernedBy({
      {.subtables = {ListOfPairs({{10, {After(20, -50), After(21, 12)}}})}},
      {.subtables = {ListOfPairs({{10, {After(20, -20), After(21, -12)}}, {11, {After(20, 5)}}})}},
  });
  // A pair two lookups move by as much each way is not kerned.
  EXPECT_EQ(ReadKerning(gpos, {}, kGlyphs).pairs, (Pairs{{10, 20, -70}, {11, 20, 5}}));
}

TEST(FontKerning, ReadsTheKerningOfEveryScriptsDefaultLanguageAndNoOtherFeature) {
  const std::vector<Lookup> lookups = {
      {.subtables = {ListOfPairs({{10, {After(20, -1)}}})}},
      {.subtables = {ListOfPairs({{11, {After(20, -2)}}})}},
      {.subtables = {ListOfPairs({{12, {After(20, -3)}}})}},
      {.subtables = {ListOfPairs({{13, {After(20, -4)}}})}},
  };
  const Bytes gpos = Gpos(
      {
          {.tag = "DFLT",
           .features = {{.tag = "mark", .lookups = {2}}, {.tag = "kern", .lookups = {0}}},
           .otherLanguage = {}},
          {.tag = "grek", .features = {{.tag = "kern", .lookups = {1, 0}}}, .otherLanguage = {}},
          {.tag = "latn", .features = {}, .otherLanguage = {}},  // No default language.
          {.tag = "cyrl", .features = {{.tag = "kerm", .lookups = {3}}}, .otherLanguage = {}},
      },
      lookups);
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  // Lookup 0 once, though two scripts name it; lookup 1; and neither of the
  // lookups that only other features name.
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -1}, {11, 20, -2}}));
  EXPECT_FALSE(kerning.damaged);

  // A feature that names a lookup there is none of names nothing.
  const Bytes beyond =
      Gpos({{.tag = "DFLT", .features = {{.tag = "kern", .lookups = {0, 9}}}, .otherLanguage = {}}}, lookups);
  EXPECT_EQ(ReadKerning(beyond, {}, kGlyphs).pairs, (Pairs{{10, 20, -1}}));
}

TEST(FontKerning, ReadsNoLanguageOfAScriptButItsDefault) {
  const std::vector<Lookup> lookups = {
      {.subtables = {ListOfPairs({{10, {After(20, -1)}}})}},
      {.subtables = {ListOfPairs({{11, {After(20, -2)}}})}},
      {.subtables = {ListOfPairs({{12, {After(20, -3)}}})}},
  };
  // One script has a default language and another besides; one has only
  // the other, and so nothing that is read.
  const Bytes gpos = Gpos(
      {
          {.tag = "latn",
           .features = {{.tag = "kern", .lookups = {0}}},
           .otherLanguage = {{.tag = "kern", .lookups = {1}}}},
          {.tag = "cyrl", .features = {}, .otherLanguage = {{.tag = "kern", .lookups = {2}}}},
      },
      lookups);
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -1}}));
  EXPECT_FALSE(kerning.damaged);
}

TEST(FontKerning, SaysNothingOfAClassATableOfClassesHasNoNumbersFor) {
  // The table has numbers for two classes of first glyphs and two of second
  // (0 and 1 of each), and its tables of classes name more: 11 is of a
  // class of first glyphs it has no row for, and 21 of a class of second
  // glyphs no row has a place for.
  const Bytes gpos =
      KernedBy({{.subtables = {TableOfClasses({10, 11, 12}, {{10, 1}, {11, 2}, {12, 7}}, {{20, 1}, {21, 2}, {22, 9}},
                                              {{0, 0}, {0, -40}}),
                               // Which a later subtable may answer for.
                               ListOfPairs({{11, {After(20, 6)}}, {10, {After(21, 7), After(20, 8)}}})}}});
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -40}, {10, 21, 7}, {11, 20, 6}}));
  EXPECT_FALSE(kerning.damaged);
}

TEST(FontKerning, SaysNothingOfAGlyphAListOfPairsCoversAndHasNoPairsFor) {
  // A list that covers 10 and 11 and says it has pairs for one glyph only.
  Bytes list = ListOfPairs({{10, {After(20, -50)}}, {11, {After(20, -10)}}});
  ASSERT_EQ(list[8], 0);
  ASSERT_EQ(list[9], 2);
  list[9] = 1;
  const FontKerning kerning = ReadKerning(KernedBy({{.subtables = {list}}}), {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -50}}));
}

TEST(FontKerning, ReadsALookupWrittenAsAnExtension) {
  const Bytes gpos = KernedBy({{.subtables = {ListOfPairs({{10, {After(20, -50)}}}),
                                              TableOfClasses({11}, {{11, 1}}, {{21, 1}}, {{0, 0}, {0, 33}})},
                                .extension = true}});
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -50}, {11, 21, 33}}));
  EXPECT_EQ(kerning.lookupsLeftOut, 0U);
}

TEST(FontKerning, LeavesOutALookupThatIsNotOfPairsAndCountsIt) {
  // A lookup of another type has subtables that are not read: whatever is
  // in them here.
  const Bytes gpos = KernedBy({
      {.type = 8, .subtables = {ListOfPairs({{10, {After(20, -99)}}})}},
      {.subtables = {ListOfPairs({{10, {After(21, -50)}}})}},
      {.type = 1, .subtables = {ListOfPairs({{10, {After(22, -98)}}})}, .extension = true},
  });
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 21, -50}}));
  EXPECT_EQ(kerning.lookupsLeftOut, 2U);
}

TEST(FontKerning, KernsNoPairWithAGlyphItsLookupPassesOver) {
  // 30 is a mark, 12 a ligature and 13 a base glyph.
  const Bytes gdef = Gdef({{10, 1}, {12, 2}, {13, 1}, {30, 3}});
  const std::map<std::uint16_t, std::vector<Second>> pairs = {
      {10, {After(20, -1), After(30, -2)}},
      {12, {After(20, -3)}},
      {30, {After(20, -4)}},
  };
  const auto with = [&](std::uint16_t flag) {
    return ReadKerning(KernedBy({{.flag = flag, .subtables = {ListOfPairs(pairs)}}}), gdef, kGlyphs).pairs;
  };
  EXPECT_EQ(with(0), (Pairs{{10, 20, -1}, {10, 30, -2}, {12, 20, -3}, {30, 20, -4}}));
  // Marks passed over: neither after one nor before one.
  EXPECT_EQ(with(0x0008), (Pairs{{10, 20, -1}, {12, 20, -3}}));
  // Some marks passed over, by a set of them or by their kind, is all of them.
  EXPECT_EQ(with(0x0010), (Pairs{{10, 20, -1}, {12, 20, -3}}));
  EXPECT_EQ(with(0x0200), (Pairs{{10, 20, -1}, {12, 20, -3}}));
  // Ligatures, and base glyphs.
  EXPECT_EQ(with(0x0004), (Pairs{{10, 20, -1}, {10, 30, -2}, {30, 20, -4}}));
  EXPECT_EQ(with(0x0002), (Pairs{{12, 20, -3}, {30, 20, -4}}));
  // With no GDEF there is nothing to tell a mark by.
  EXPECT_EQ(ReadKerning(KernedBy({{.flag = 0x0008, .subtables = {ListOfPairs(pairs)}}}), {}, kGlyphs).pairs,
            (Pairs{{10, 20, -1}, {10, 30, -2}, {12, 20, -3}, {30, 20, -4}}));

  // The same of a table of classes.
  const Bytes table = TableOfClasses({10, 30}, {{10, 1}, {30, 1}}, {{20, 1}, {30, 1}}, {{0, 0}, {0, -6}});
  EXPECT_EQ(ReadKerning(KernedBy({{.flag = 0, .subtables = {table}}}), gdef, kGlyphs).pairs,
            (Pairs{{10, 20, -6}, {10, 30, -6}, {30, 20, -6}, {30, 30, -6}}));
  EXPECT_EQ(ReadKerning(KernedBy({{.flag = 0x0008, .subtables = {table}}}), gdef, kGlyphs).pairs,
            (Pairs{{10, 20, -6}}));
}

TEST(FontKerning, ReadsTheFirstGlyphsAdvanceAndCountsWhatElseAPairAdjusts) {
  // A record with everything in it: the advance is the third number.
  const std::uint16_t all = kXPlacement | kYPlacement | kXAdvance | kYAdvance;
  const Bytes gpos =
      KernedBy({{.subtables = {ListOfPairs(
                     {
                         {10,
                          {
                              {.glyph = 20,
                               .first = {.xPlacement = 0, .yPlacement = 9, .xAdvance = -31, .yAdvance = 8},
                               .second = {}},
                              {.glyph = 21,
                               .first = {.xPlacement = 7, .yPlacement = 0, .xAdvance = -32, .yAdvance = 0},
                               .second = {}},
                              {.glyph = 22, .first = {.xAdvance = -33}, .second = {.xPlacement = -4}},
                          }},
                         {11, {{.glyph = 20, .first = {.xAdvance = -34}, .second = {.xAdvance = 2}}}},
                         // Moved, and its advance left as it is: no kerning here.
                         {12, {{.glyph = 20, .first = {.xPlacement = 5}, .second = {}}}},
                     },
                     all, all)}}});
  const FontKerning kerning = ReadKerning(gpos, {}, kGlyphs);
  EXPECT_EQ(kerning.pairs, (Pairs{{10, 20, -31}, {10, 21, -32}, {10, 22, -33}, {11, 20, -34}}));
  // Up and down is not counted: across is, of either glyph, and the
  // second's advance.
  EXPECT_EQ(kerning.otherAdjustments, 4U);

  // A record with the advance alone after a placement up or down.
  const Bytes two = KernedBy(
      {{.subtables = {ListOfPairs({{10, {{.glyph = 20, .first = {.yPlacement = 60, .xAdvance = -35}, .second = {}}}}},
                                  kYPlacement | kXAdvance, 0)}}});
  EXPECT_EQ(ReadKerning(two, {}, kGlyphs).pairs, (Pairs{{10, 20, -35}}));
  // And one with no advance in it at all.
  const Bytes none =
      KernedBy({{.subtables = {ListOfPairs({{10, {{.glyph = 20, .first = {.xPlacement = 60}, .second = {}}}}},
                                           kXPlacement, 0)}}});
  EXPECT_TRUE(ReadKerning(none, {}, kGlyphs).pairs.empty());
}

TEST(FontKerning, HasNoneWithoutATableOrWithoutGlyphs) {
  EXPECT_TRUE(ReadKerning({}, {}, kGlyphs).pairs.empty());
  EXPECT_FALSE(ReadKerning({}, {}, kGlyphs).damaged);
  const Bytes gpos = KernedBy({{.subtables = {ListOfPairs({{10, {After(20, -50)}}})}}});
  EXPECT_TRUE(ReadKerning(gpos, {}, {}).pairs.empty());
  // A table with no scripts, and one with no lookups for its feature.
  EXPECT_TRUE(ReadKerning(Gpos({}, {}), {}, kGlyphs).pairs.empty());
  EXPECT_TRUE(
      ReadKerning(Gpos({{.tag = "DFLT", .features = {{.tag = "kern", .lookups = {}}}, .otherLanguage = {}}}, {}), {},
                  kGlyphs)
          .pairs.empty());
}

// A table with a little of everything in it.
Bytes MixedGpos() {
  return Gpos(
      {
          {.tag = "DFLT", .features = {{.tag = "kern", .lookups = {0, 1}}}, .otherLanguage = {}},
          {.tag = "latn", .features = {{.tag = "kern", .lookups = {2}}}, .otherLanguage = {}},
      },
      {
          {.flag = 0x0008,
           .subtables = {ListOfPairs({{10, {After(20, -50), After(21, 30)}}, {11, {After(20, -10)}}}),
                         TableOfClasses({10, 11, 12}, {{10, 1}, {11, 1}, {12, 2}}, {{20, 1}, {21, 2}},
                                        {{0, 0, 0}, {0, -40, 15}, {-3, 0, -25}})}},
          {.subtables = {ListOfPairs({{12, {After(22, 8)}}}, kXPlacement | kXAdvance, kXAdvance, true)},
           .extension = true},
          {.subtables = {TableOfClasses({13}, {{13, 1}}, {{30, 1}}, {{0, 0}, {0, 21}}, true)}},
      });
}

TEST(FontKerning, ReadsNothingFromOutsideATableThatIsCutShort) {
  const Bytes good = MixedGpos();
  const Bytes gdef = Gdef({{30, 3}});
  const FontKerning whole = ReadKerning(good, gdef, kGlyphs);
  ASSERT_FALSE(whole.damaged);
  ASSERT_GT(whole.pairs.size(), 8U);
  std::size_t damaged = 0;
  for (std::size_t length = 0; length < good.size(); ++length) {
    // In memory of its own size, so that a byte read past the cut is seen.
    const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    const FontKerning kerning = ReadKerning(cut, gdef, kGlyphs);
    damaged += kerning.damaged ? 1U : 0U;
    // Whatever it made of what was left, it is of the glyphs asked about.
    for (const KernedPair& pair : kerning.pairs) {
      ASSERT_TRUE(std::find(kGlyphs.begin(), kGlyphs.end(), pair.first) != kGlyphs.end()) << length;
      ASSERT_TRUE(std::find(kGlyphs.begin(), kGlyphs.end(), pair.second) != kGlyphs.end()) << length;
    }
  }
  // All but the shortest, which have not so much as a script, are seen to
  // be cut.
  EXPECT_GT(damaged, good.size() - 12);
  for (std::size_t length = 0; length < gdef.size(); ++length) {
    const Bytes cut(gdef.begin(), gdef.begin() + static_cast<std::ptrdiff_t>(length));
    static_cast<void>(ReadKerning(good, cut, kGlyphs));
  }
}

TEST(FontKerning, ReadsNothingFromOutsideATableWithAnyByteChanged) {
  const Bytes good = MixedGpos();
  const Bytes gdef = Gdef({{30, 3}});
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      Bytes spoiled = good;
      spoiled[at] ^= change;
      const FontKerning kerning = ReadKerning(spoiled, gdef, kGlyphs);
      ASSERT_LE(kerning.pairs.size(), kGlyphs.size() * kGlyphs.size()) << at;
    }
  }
}

// ---- A font's pairs as a face's ----

std::vector<std::vector<std::int32_t>> AsNumbers(const std::vector<FacePair>& pairs) {
  std::vector<std::vector<std::int32_t>> numbers;
  numbers.reserve(pairs.size());
  for (const FacePair& pair : pairs) {
    numbers.push_back({static_cast<std::int32_t>(pair.first), static_cast<std::int32_t>(pair.second), pair.quarters});
  }
  return numbers;
}

TEST(FacePairs, PutsAPairToTheNearestQuarterOfAPixelAHalfGoingAwayFromNothing) {
  // The face's glyphs 1 to 4 are drawn from the font's 10 to 13. At 25
  // pixels to an em of 1,000 units a quarter of a pixel is 10 units: 14
  // units are one quarter, 15 two, and 4 none; and so the other way.
  const std::vector<std::uint32_t> drawnFrom = {0, 10, 11, 12, 13};
  FontKerning kerning;
  kerning.pairs = {{10, 11, 14}, {10, 12, 15}, {10, 13, 4},  {11, 10, -14}, {11, 12, -15},
                   {11, 13, -4}, {12, 10, 5},  {12, 11, -5}, {13, 13, -160}};
  using Numbers = std::vector<std::vector<std::int32_t>>;
  EXPECT_EQ(AsNumbers(bn::font_tool::FacePairsOf(kerning, drawnFrom, 25, 1000)),
            (Numbers{{1, 2, 1}, {1, 3, 2}, {2, 1, -1}, {2, 3, -2}, {3, 1, 1}, {3, 2, -1}, {4, 4, -16}}));
  // Twice the size, twice as far; and an em of other units.
  EXPECT_EQ(AsNumbers(bn::font_tool::FacePairsOf(kerning, drawnFrom, 50, 1000))[0],
            (std::vector<std::int32_t>{1, 2, 3}));
  EXPECT_EQ(AsNumbers(bn::font_tool::FacePairsOf(kerning, drawnFrom, 25, 2000)),
            (Numbers{{1, 2, 1}, {1, 3, 1}, {2, 1, -1}, {2, 3, -1}, {4, 4, -8}}));
  // A font that does not say how many units it has to the em kerns nothing.
  EXPECT_TRUE(bn::font_tool::FacePairsOf(kerning, drawnFrom, 25, 0).empty());
}

TEST(FacePairs, KernsEveryGlyphOfAFaceThatIsDrawnFromAGlyphOfTheFont) {
  // The font draws two characters with its glyph 10 and three with its 11:
  // the face's 1 and 4, and its 2, 3 and 6. Its glyph 5 is drawn from one
  // the font kerns with nothing, and its glyph 0 is what it draws for what
  // it lacks, which is the font's glyph 0.
  const std::vector<std::uint32_t> drawnFrom = {0, 10, 11, 11, 10, 12, 11};
  FontKerning kerning;
  kerning.pairs = {{0, 10, -99}, {10, 0, -99}, {10, 11, -40}, {11, 11, 20}, {13, 10, -40}, {10, 13, -40}};
  using Numbers = std::vector<std::vector<std::int32_t>>;
  EXPECT_EQ(AsNumbers(bn::font_tool::FacePairsOf(kerning, drawnFrom, 25, 1000)), (Numbers{{1, 2, -4},
                                                                                          {1, 3, -4},
                                                                                          {1, 6, -4},
                                                                                          {4, 2, -4},
                                                                                          {4, 3, -4},
                                                                                          {4, 6, -4},
                                                                                          {2, 2, 2},
                                                                                          {2, 3, 2},
                                                                                          {2, 6, 2},
                                                                                          {3, 2, 2},
                                                                                          {3, 3, 2},
                                                                                          {3, 6, 2},
                                                                                          {6, 2, 2},
                                                                                          {6, 3, 2},
                                                                                          {6, 6, 2}}));
  // Which come to two classes of first glyphs and one of second.
  const KerningClasses classes =
      MakeKerningClasses(drawnFrom.size(), bn::font_tool::FacePairsOf(kerning, drawnFrom, 25, 1000));
  EXPECT_EQ(classes.firstOf, (std::vector<std::uint8_t>{0, 1, 2, 2, 1, 0, 2}));
  EXPECT_EQ(classes.secondOf, (std::vector<std::uint8_t>{0, 0, 1, 1, 0, 0, 1}));
  EXPECT_EQ(classes.table, (std::vector<std::int8_t>{-4, 2}));
}

// ---- Classes ----

// What `classes` kerns glyph `first` followed by `second` by.
int Kerned(const KerningClasses& classes, std::size_t first, std::size_t second) {
  const std::size_t row = classes.firstOf[first];
  const std::size_t column = classes.secondOf[second];
  if (row == 0 || column == 0) {
    return 0;
  }
  return classes.table[((row - 1) * classes.seconds) + (column - 1)];
}

TEST(KerningClasses, PutsGlyphsThatKernAlikeInOneClass) {
  // 1 and 2 kern alike with what follows, and 3 otherwise; 5 and 6 are
  // kerned differently after them.
  const std::vector<FacePair> pairs = {{1, 5, -4}, {2, 5, -4}, {1, 6, 2}, {2, 6, 2}, {3, 5, -8}};
  const KerningClasses classes = MakeKerningClasses(8, pairs);
  ASSERT_TRUE(classes.fits);
  EXPECT_EQ(classes.pairs, 5U);
  EXPECT_EQ(classes.firsts, 2U);
  EXPECT_EQ(classes.seconds, 2U);
  // Numbered from 1 as the glyphs come; 0 for a glyph that does not kern.
  EXPECT_EQ(classes.firstOf, (std::vector<std::uint8_t>{0, 1, 1, 2, 0, 0, 0, 0}));
  EXPECT_EQ(classes.secondOf, (std::vector<std::uint8_t>{0, 0, 0, 0, 0, 1, 2, 0}));
  // A row for each class of first glyphs, a number in it for each of second.
  EXPECT_EQ(classes.table, (std::vector<std::int8_t>{-4, 2, -8, 0}));
  EXPECT_EQ(classes.clipped, 0U);
}

TEST(KerningClasses, KernsEveryPairAsItWasGivenAndNoOther) {
  // Pairs by a rule that makes glyphs kern alike in groups, and a few that
  // break the groups.
  const std::size_t glyphs = 60;
  std::map<std::pair<std::size_t, std::size_t>, int> given;
  for (std::size_t first = 1; first < 40; ++first) {
    for (std::size_t second = 20; second < glyphs; ++second) {
      const int quarters = static_cast<int>(((first % 5) * 7) + ((second % 4) * 3)) - 20;
      if ((first % 5) != 0 && (second % 4) != 1 && quarters != 0) {
        given[{first, second}] = quarters;
      }
    }
  }
  given[{7, 33}] = 99;
  given[{12, 21}] = -77;
  given.erase({3, 22});
  std::vector<FacePair> pairs;
  pairs.reserve(given.size());
  for (const auto& [pair, quarters] : given) {
    pairs.push_back({static_cast<std::uint32_t>(pair.first), static_cast<std::uint32_t>(pair.second), quarters});
  }
  const KerningClasses classes = MakeKerningClasses(glyphs, pairs);
  ASSERT_TRUE(classes.fits);
  EXPECT_EQ(classes.pairs, given.size());
  ASSERT_EQ(classes.firstOf.size(), glyphs);
  ASSERT_EQ(classes.secondOf.size(), glyphs);
  ASSERT_EQ(classes.table.size(), classes.firsts * classes.seconds);
  for (std::size_t first = 0; first < glyphs; ++first) {
    ASSERT_LE(classes.firstOf[first], classes.firsts);
    ASSERT_LE(classes.secondOf[first], classes.seconds);
    for (std::size_t second = 0; second < glyphs; ++second) {
      const auto found = given.find({first, second});
      ASSERT_EQ(Kerned(classes, first, second), found == given.end() ? 0 : found->second) << first << " " << second;
    }
  }
  // Far fewer classes than glyphs: four ways of kerning as a first and
  // three as a second, and the three glyphs of each kind that were set
  // apart from their groups.
  EXPECT_EQ(classes.firsts, 7U);
  EXPECT_EQ(classes.seconds, 6U);
}

TEST(KerningClasses, TakesAPairAsItIsNamedLastAndOneOfNothingAsNoPair) {
  const std::vector<FacePair> pairs = {{1, 2, -4}, {1, 3, 6}, {1, 2, -9}, {1, 3, 0}, {2, 2, 0}};
  const KerningClasses classes = MakeKerningClasses(4, pairs);
  ASSERT_TRUE(classes.fits);
  EXPECT_EQ(classes.pairs, 1U);
  EXPECT_EQ(classes.firsts, 1U);
  EXPECT_EQ(classes.seconds, 1U);
  EXPECT_EQ(Kerned(classes, 1, 2), -9);
  EXPECT_EQ(Kerned(classes, 1, 3), 0);
  // A glyph the face has not is of no pair.
  const std::vector<FacePair> beyond = {{1, 4, -4}, {4, 1, -4}, {9, 9, 3}, {1, 2, 5}};
  const KerningClasses kept = MakeKerningClasses(4, beyond);
  EXPECT_EQ(kept.pairs, 1U);
  EXPECT_EQ(Kerned(kept, 1, 2), 5);
}

TEST(KerningClasses, HasNoTableForAFaceThatDoesNotKern) {
  const KerningClasses classes = MakeKerningClasses(5, {});
  EXPECT_TRUE(classes.fits);
  EXPECT_EQ(classes.pairs, 0U);
  EXPECT_EQ(classes.firsts, 0U);
  EXPECT_EQ(classes.seconds, 0U);
  EXPECT_TRUE(classes.table.empty());
  EXPECT_EQ(classes.firstOf, (std::vector<std::uint8_t>(5, 0)));
  EXPECT_TRUE(MakeKerningClasses(0, {}).fits);
}

TEST(KerningClasses, HoldsAPairKernedByMoreThanAByteSaysAtTheMostItDoes) {
  const std::vector<FacePair> pairs = {{1, 2, 127}, {1, 3, 128}, {2, 2, -128}, {2, 3, -129}, {3, 3, 4000}};
  const KerningClasses classes = MakeKerningClasses(4, pairs);
  ASSERT_TRUE(classes.fits);
  EXPECT_EQ(Kerned(classes, 1, 2), 127);
  EXPECT_EQ(Kerned(classes, 1, 3), 127);
  EXPECT_EQ(Kerned(classes, 2, 2), -128);
  EXPECT_EQ(Kerned(classes, 2, 3), -128);
  EXPECT_EQ(Kerned(classes, 3, 3), 127);
  EXPECT_EQ(classes.clipped, 3U);
}

TEST(KerningClasses, DoesNotFitWithMoreClassesThanAByteCounts) {
  // Each glyph kerns with the next by an amount of its own: as many classes
  // of first glyphs as glyphs that kern.
  const auto chain = [](std::uint32_t links) {
    std::vector<FacePair> pairs;
    pairs.reserve(links);
    for (std::uint32_t first = 0; first < links; ++first) {
      pairs.push_back({first, first + 1, static_cast<std::int32_t>(first % 100) + 1});
      // And each second glyph is told apart by who kerns with it.
    }
    return pairs;
  };
  const KerningClasses most = MakeKerningClasses(300, chain(255));
  EXPECT_TRUE(most.fits);
  EXPECT_EQ(most.firsts, 255U);
  EXPECT_EQ(most.seconds, 255U);
  EXPECT_EQ(most.table.size(), 255U * 255U);
  EXPECT_EQ(Kerned(most, 254, 255), 55);

  const KerningClasses over = MakeKerningClasses(300, chain(256));
  EXPECT_FALSE(over.fits);
  EXPECT_EQ(over.firsts, 256U);
  EXPECT_EQ(over.seconds, 256U);
  EXPECT_EQ(over.pairs, 256U);
  EXPECT_TRUE(over.table.empty());
  EXPECT_TRUE(over.firstOf.empty());
}

}  // namespace
