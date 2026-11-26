#include "font/kerning.h"

#include <algorithm>
#include <bit>
#include <map>
#include <utility>

#include "font/opentype.h"

namespace bn::font_tool {

namespace {

// How many bytes a value record of `format` takes: two for each thing it
// has, of the eight there are.
std::size_t ValueBytes(std::uint16_t format) {
  return std::size_t{2} * static_cast<std::size_t>(std::popcount(format & 0xFFU));
}

// Of the value record at `at`, how far it moves a glyph across and how
// much it adds to the glyph's advance.
struct Value {
  std::int32_t xPlacement = 0;
  std::int32_t xAdvance = 0;
};
Value ReadValue(const Table& table, std::size_t at, std::uint16_t format) {
  Value value;
  if ((format & 0x0001U) != 0) {
    value.xPlacement = table.S16(at);
    at += 2;
  }
  if ((format & 0x0002U) != 0) {
    at += 2;  // Up or down.
  }
  if ((format & 0x0004U) != 0) {
    value.xAdvance = table.S16(at);
  }
  return value;
}

// One subtable of pair adjustments.
struct PairSubtable {
  std::size_t at = 0;
  std::uint16_t format = 0;
  std::uint16_t valueFormat1 = 0;
  std::uint16_t valueFormat2 = 0;
  std::vector<std::int32_t> coverage;
  // A table of classes has these, and a list of pairs has not.
  std::vector<std::uint16_t> class1;
  std::vector<std::uint16_t> class2;
  std::uint16_t class1Count = 0;
  std::uint16_t class2Count = 0;
};

struct PairLookup {
  std::uint16_t flag = 0;
  std::vector<PairSubtable> subtables;
};

// The classes GDEF puts glyphs in.
constexpr std::uint16_t kBaseGlyph = 1;
constexpr std::uint16_t kLigature = 2;
constexpr std::uint16_t kMark = 3;

// Whether a lookup with `flag` passes over a glyph GDEF has in `glyphClass`.
// A lookup that passes over some marks and not others is taken to pass over
// them all: which marks is for marks set on letters, not for kerning.
bool PassedOver(std::uint16_t flag, std::uint16_t glyphClass) {
  constexpr unsigned kIgnoreBaseGlyphs = 0x0002;
  constexpr unsigned kIgnoreLigatures = 0x0004;
  constexpr unsigned kIgnoreMarks = 0x0008;
  constexpr unsigned kSomeMarks = 0xFF10;  // A set of marks, or a kind of them.
  return (glyphClass == kBaseGlyph && (flag & kIgnoreBaseGlyphs) != 0) ||
         (glyphClass == kLigature && (flag & kIgnoreLigatures) != 0) ||
         (glyphClass == kMark && (flag & (kIgnoreMarks | kSomeMarks)) != 0);
}

// Reads lookup `index` into `lookup`. False if it is not one of pair
// adjustments alone.
bool ReadPairLookup(const Table& gpos, std::uint16_t index, PairLookup& lookup) {
  constexpr std::uint16_t kPairAdjustment = 2;
  constexpr std::uint16_t kExtension = 9;
  const std::size_t list = gpos.U16(8);
  if (list == 0 || index >= gpos.U16(list)) {
    return false;
  }
  const std::size_t at = list + gpos.U16(list + 2 + (std::size_t{2} * index));
  const std::uint16_t type = gpos.U16(at);
  lookup.flag = gpos.U16(at + 2);
  const std::uint16_t count = gpos.U16(at + 4);
  for (std::size_t i = 0; i < count && !gpos.Damaged(); ++i) {
    std::size_t sub = at + gpos.U16(at + 6 + (2 * i));
    std::uint16_t kind = type;
    if (type == kExtension) {
      // An extension is a longer way to say where a subtable is.
      kind = gpos.U16(sub + 2);
      sub += gpos.U32(sub + 4);
    }
    if (kind != kPairAdjustment) {
      return false;
    }
    PairSubtable pairs;
    pairs.at = sub;
    pairs.format = gpos.U16(sub);
    pairs.valueFormat1 = gpos.U16(sub + 4);
    pairs.valueFormat2 = gpos.U16(sub + 6);
    pairs.coverage = Coverage(gpos, sub + gpos.U16(sub + 2));
    if (pairs.format == 2) {
      pairs.class1 = Classes(gpos, sub + gpos.U16(sub + 8));
      pairs.class2 = Classes(gpos, sub + gpos.U16(sub + 10));
      pairs.class1Count = gpos.U16(sub + 12);
      pairs.class2Count = gpos.U16(sub + 14);
    } else if (pairs.format != 1) {
      continue;  // A format there is none of says nothing.
    }
    lookup.subtables.push_back(std::move(pairs));
  }
  return true;
}

// What one row of kerning is gathered in: for the glyph that comes first,
// what each of the glyphs asked about is moved by after it.
struct Row {
  std::vector<std::int32_t> units;
  std::vector<std::uint8_t> taken;  // By the lookup being gone through.
  std::size_t otherAdjustments = 0;

  void Add(std::size_t second, Value first, Value other) {
    taken[second] = 1;
    units[second] += first.xAdvance;
    if (first.xPlacement != 0 || other.xPlacement != 0 || other.xAdvance != 0) {
      ++otherAdjustments;
    }
  }
};

}  // namespace

FontKerning ReadKerning(std::span<const std::uint8_t> gposBytes, std::span<const std::uint8_t> gdefBytes,
                        std::span<const std::uint16_t> glyphs) {
  FontKerning kerning;
  const Table gpos(gposBytes, kerning.damaged);
  const Table gdef(gdefBytes, kerning.damaged);
  if (gpos.Empty()) {
    return kerning;
  }
  std::vector<PairLookup> lookups;
  constexpr std::uint32_t kKern = 0x6B65726EU;  // "kern"
  for (const std::uint16_t index : FeatureLookups(gpos, kKern)) {
    PairLookup lookup;
    if (ReadPairLookup(gpos, index, lookup)) {
      lookups.push_back(std::move(lookup));
    } else {
      ++kerning.lookupsLeftOut;
    }
  }
  // The classes GDEF has glyphs in, for what a lookup passes over.
  std::vector<std::uint16_t> glyphClass(kGlyphNumbers, 0);
  if (!gdef.Empty() && gdef.U16(4) != 0) {
    glyphClass = Classes(gdef, gdef.U16(4));
  }
  // Where each glyph asked about is in `glyphs`.
  std::vector<std::int32_t> placeOf(kGlyphNumbers, -1);
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    placeOf[glyphs[i]] = static_cast<std::int32_t>(i);
  }

  Row row;
  for (const std::uint16_t first : glyphs) {
    row.units.assign(glyphs.size(), 0);
    for (const PairLookup& lookup : lookups) {
      if (PassedOver(lookup.flag, glyphClass[first])) {
        continue;
      }
      row.taken.assign(glyphs.size(), 0);
      for (const PairSubtable& sub : lookup.subtables) {
        const std::int32_t place = sub.coverage[first];
        if (place < 0 || gpos.Damaged()) {
          continue;
        }
        const std::size_t bytes1 = ValueBytes(sub.valueFormat1);
        const std::size_t bytes2 = ValueBytes(sub.valueFormat2);
        if (sub.format == 1) {
          // The pairs that begin with this glyph, each with its second.
          if (place >= gpos.U16(sub.at + 8)) {
            continue;
          }
          const std::size_t set = sub.at + gpos.U16(sub.at + 10 + (std::size_t{2} * static_cast<std::size_t>(place)));
          const std::uint16_t count = gpos.U16(set);
          for (std::size_t i = 0; i < count && !gpos.Damaged(); ++i) {
            const std::size_t record = set + 2 + (i * (2 + bytes1 + bytes2));
            const std::uint16_t second = gpos.U16(record);
            const std::int32_t at = placeOf[second];
            if (at < 0 || row.taken[static_cast<std::size_t>(at)] != 0 || PassedOver(lookup.flag, glyphClass[second])) {
              continue;
            }
            row.Add(static_cast<std::size_t>(at), ReadValue(gpos, record + 2, sub.valueFormat1),
                    ReadValue(gpos, record + 2 + bytes1, sub.valueFormat2));
          }
        } else {
          // A number for this glyph's class with every class of second.
          const std::uint16_t class1 = sub.class1[first];
          if (class1 >= sub.class1Count) {
            continue;
          }
          for (std::size_t at = 0; at < glyphs.size() && !gpos.Damaged(); ++at) {
            const std::uint16_t class2 = sub.class2[glyphs[at]];
            if (row.taken[at] != 0 || class2 >= sub.class2Count || PassedOver(lookup.flag, glyphClass[glyphs[at]])) {
              continue;
            }
            const std::size_t record =
                sub.at + 16 + (((std::size_t{class1} * sub.class2Count) + class2) * (bytes1 + bytes2));
            row.Add(at, ReadValue(gpos, record, sub.valueFormat1), ReadValue(gpos, record + bytes1, sub.valueFormat2));
          }
        }
      }
    }
    for (std::size_t at = 0; at < glyphs.size(); ++at) {
      if (row.units[at] != 0) {
        kerning.pairs.push_back({.first = first, .second = glyphs[at], .units = row.units[at]});
      }
    }
  }
  kerning.otherAdjustments = row.otherAdjustments;
  return kerning;
}

std::vector<FacePair> FacePairsOf(const FontKerning& kerning, std::span<const std::uint32_t> drawnFrom,
                                  std::uint16_t size, std::uint32_t unitsToTheEm) {
  std::vector<FacePair> pairs;
  if (unitsToTheEm == 0) {
    return pairs;
  }
  // The glyphs of the face that each of the font's glyphs is drawn as.
  std::map<std::uint32_t, std::vector<std::uint32_t>> drawnAs;
  for (std::size_t glyph = 1; glyph < drawnFrom.size(); ++glyph) {
    drawnAs[drawnFrom[glyph]].push_back(static_cast<std::uint32_t>(glyph));
  }
  for (const KernedPair& pair : kerning.pairs) {
    // Eighths of a pixel, so that a half is added before the halving.
    const std::int64_t far = std::int64_t{pair.units < 0 ? -pair.units : pair.units} * 8 * size;
    const auto quarters = static_cast<std::int32_t>((far + unitsToTheEm) / (std::int64_t{2} * unitsToTheEm));
    const auto firsts = drawnAs.find(pair.first);
    const auto seconds = drawnAs.find(pair.second);
    if (quarters == 0 || firsts == drawnAs.end() || seconds == drawnAs.end()) {
      continue;
    }
    for (const std::uint32_t first : firsts->second) {
      for (const std::uint32_t second : seconds->second) {
        pairs.push_back({.first = first, .second = second, .quarters = pair.units < 0 ? -quarters : quarters});
      }
    }
  }
  return pairs;
}

KerningClasses MakeKerningClasses(std::size_t glyphs, std::span<const FacePair> pairs) {
  KerningClasses classes;
  // Each pair once, as it is named last; then those that are kerned at
  // all, held to what a signed byte holds.
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> named;
  for (const FacePair& pair : pairs) {
    if (pair.first < glyphs && pair.second < glyphs) {
      named[{pair.first, pair.second}] = pair.quarters;
    }
  }
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::int8_t> kerned;
  for (const auto& [pair, quarters] : named) {
    if (quarters == 0) {
      continue;
    }
    const std::int32_t held = std::clamp(quarters, -128, 127);
    classes.clipped += held != quarters ? 1U : 0U;
    kerned[pair] = static_cast<std::int8_t>(held);
  }
  classes.pairs = kerned.size();

  // What each glyph does to those after it, and what is done to it by
  // those before: `kerned` is in order, so each list is.
  using Others = std::vector<std::pair<std::uint32_t, std::int8_t>>;
  std::vector<Others> after(glyphs);
  std::vector<Others> before(glyphs);
  for (const auto& [pair, quarters] : kerned) {
    after[pair.first].emplace_back(pair.second, quarters);
    before[pair.second].emplace_back(pair.first, quarters);
  }
  // Glyphs with the same list are one class, numbered from 1 as they come.
  const auto number = [glyphs](const std::vector<Others>& lists, std::vector<std::size_t>& of) {
    std::map<Others, std::size_t> seen;
    of.assign(glyphs, 0);
    for (std::size_t glyph = 0; glyph < glyphs; ++glyph) {
      if (!lists[glyph].empty()) {
        of[glyph] = seen.try_emplace(lists[glyph], seen.size() + 1).first->second;
      }
    }
    return seen.size();
  };
  std::vector<std::size_t> firstOf;
  std::vector<std::size_t> secondOf;
  classes.firsts = number(after, firstOf);
  classes.seconds = number(before, secondOf);
  constexpr std::size_t kMostClasses = 255;
  if (classes.firsts > kMostClasses || classes.seconds > kMostClasses) {
    classes.fits = false;
    return classes;
  }
  classes.firstOf.assign(firstOf.begin(), firstOf.end());
  classes.secondOf.assign(secondOf.begin(), secondOf.end());
  classes.table.assign(classes.firsts * classes.seconds, 0);
  for (const auto& [pair, quarters] : kerned) {
    classes.table[((firstOf[pair.first] - 1) * classes.seconds) + (secondOf[pair.second] - 1)] = quarters;
  }
  return classes;
}

}  // namespace bn::font_tool
