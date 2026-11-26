#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Writing the tables a font keeps its kerning and its ligatures in (GPOS
// and GSUB of the OpenType specification), a number at a time from the
// description of their layout, for the tests of the font builder's reading
// of them. Nothing here shares the builder's code.

namespace bn::test {

using TableBytes = std::vector<std::uint8_t>;

inline void Put16(TableBytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}
inline void Put32(TableBytes& out, std::uint32_t value) {
  Put16(out, value >> 16U);
  Put16(out, value & 0xFFFFU);
}
inline void Set16(TableBytes& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<std::uint8_t>(value >> 8U);
  out[at + 1] = static_cast<std::uint8_t>(value);
}
inline void Append(TableBytes& out, const TableBytes& more) { out.insert(out.end(), more.begin(), more.end()); }

// A coverage table of `glyphs`, which are in order: as a list, or as ranges
// of glyphs that follow one another.
inline TableBytes Coverage(const std::vector<std::uint16_t>& glyphs, bool asRanges = false) {
  TableBytes out;
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
inline TableBytes Classes(const std::map<std::uint16_t, std::uint16_t>& classOf, bool asList = false) {
  TableBytes out;
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

// A lookup: its type, its flag, and its subtables. One written as an
// extension has the type the table has for those, and says where each
// subtable of `type` is.
struct Lookup {
  std::uint16_t type = 2;
  std::uint16_t flag = 0;
  std::vector<TableBytes> subtables;
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

inline void PutTag(TableBytes& out, const std::string& tag) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(i < tag.size() ? tag[i] : ' '));
  }
}

// A GPOS or a GSUB table of `scripts` and `lookups`, which are laid out
// alike: `extensionType` is the type a lookup written as an extension has,
// 9 in GPOS and 7 in GSUB.
inline TableBytes LayoutTable(const std::vector<Script>& scripts, const std::vector<Lookup>& lookups,
                              std::uint16_t extensionType) {
  // Every feature of every script, in order: a script's are numbered from
  // where the ones before it end.
  std::vector<Feature> features;
  for (const Script& script : scripts) {
    features.insert(features.end(), script.features.begin(), script.features.end());
    features.insert(features.end(), script.otherLanguage.begin(), script.otherLanguage.end());
  }
  TableBytes out;
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
    TableBytes record;
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
    TableBytes record;
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
    Put16(out, lookup.extension ? extensionType : lookup.type);
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

}  // namespace bn::test
