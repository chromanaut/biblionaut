#include "font/opentype.h"

namespace bn::font_tool {

std::vector<std::int32_t> Coverage(const Table& table, std::size_t at) {
  std::vector<std::int32_t> places(kGlyphNumbers, -1);
  const std::uint16_t format = table.U16(at);
  const std::uint16_t count = table.U16(at + 2);
  for (std::size_t i = 0; i < count && !table.Damaged(); ++i) {
    if (format == 1) {
      places[table.U16(at + 4 + (2 * i))] = static_cast<std::int32_t>(i);
    } else if (format == 2) {
      const std::size_t range = at + 4 + (6 * i);
      const std::uint32_t first = table.U16(range);
      const std::uint32_t last = table.U16(range + 2);
      const std::uint32_t place = table.U16(range + 4);
      for (std::uint32_t glyph = first; glyph <= last; ++glyph) {
        places[glyph] = static_cast<std::int32_t>(place + (glyph - first));
      }
    }
  }
  return places;
}

std::vector<std::uint16_t> Classes(const Table& table, std::size_t at) {
  std::vector<std::uint16_t> classes(kGlyphNumbers, 0);
  const std::uint16_t format = table.U16(at);
  if (format == 1) {
    const std::uint32_t first = table.U16(at + 2);
    const std::uint16_t count = table.U16(at + 4);
    for (std::uint32_t i = 0; i < count && first + i < kGlyphNumbers && !table.Damaged(); ++i) {
      classes[first + i] = table.U16(at + 6 + (std::size_t{2} * i));
    }
  } else if (format == 2) {
    const std::uint16_t count = table.U16(at + 2);
    for (std::size_t i = 0; i < count && !table.Damaged(); ++i) {
      const std::size_t range = at + 4 + (6 * i);
      const std::uint32_t first = table.U16(range);
      const std::uint32_t last = table.U16(range + 2);
      const std::uint16_t value = table.U16(range + 4);
      for (std::uint32_t glyph = first; glyph <= last; ++glyph) {
        classes[glyph] = value;
      }
    }
  }
  return classes;
}

std::set<std::uint16_t> FeatureLookups(const Table& table, std::uint32_t tag) {
  std::set<std::uint16_t> lookups;
  const std::size_t scripts = table.U16(4);
  const std::size_t features = table.U16(6);
  if (scripts == 0 || features == 0) {
    return lookups;
  }
  const std::uint16_t scriptCount = table.U16(scripts);
  const std::uint16_t featureCount = table.U16(features);
  for (std::size_t s = 0; s < scriptCount && !table.Damaged(); ++s) {
    const std::size_t script = scripts + table.U16(scripts + 2 + (6 * s) + 4);
    const std::uint16_t byDefault = table.U16(script);
    if (byDefault == 0) {
      continue;
    }
    const std::size_t language = script + byDefault;
    const std::uint16_t count = table.U16(language + 4);
    for (std::size_t f = 0; f < count && !table.Damaged(); ++f) {
      const std::uint16_t index = table.U16(language + 6 + (2 * f));
      if (index >= featureCount) {
        continue;
      }
      const std::size_t record = features + 2 + (std::size_t{6} * index);
      if (table.U32(record) != tag) {
        continue;
      }
      const std::size_t feature = features + table.U16(record + 4);
      const std::uint16_t lookupCount = table.U16(feature + 2);
      for (std::size_t l = 0; l < lookupCount && !table.Damaged(); ++l) {
        lookups.insert(table.U16(feature + 4 + (2 * l)));
      }
    }
  }
  return lookups;
}

}  // namespace bn::font_tool
