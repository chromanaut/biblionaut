#include "font/ligatures.h"

#include <algorithm>
#include <map>
#include <utility>

#include "font/opentype.h"

namespace bn::font_tool {

FontLigatures ReadLigatures(std::span<const std::uint8_t> gsubBytes, std::span<const std::uint16_t> glyphs) {
  constexpr std::uint32_t kLiga = 0x6C696761U;  // "liga"
  constexpr std::uint16_t kLigatureSubstitution = 4;
  constexpr std::uint16_t kExtension = 7;
  FontLigatures found;
  const Table gsub(gsubBytes, found.damaged);
  if (gsub.Empty()) {
    return found;
  }
  std::vector<bool> asked(kGlyphNumbers, false);
  for (const std::uint16_t glyph : glyphs) {
    asked[glyph] = true;
  }
  // The glyphs the lookups gone through so far make.
  std::vector<bool> madeBefore(kGlyphNumbers, false);
  const std::size_t list = gsub.U16(8);
  for (const std::uint16_t index : FeatureLookups(gsub, kLiga)) {
    if (list == 0 || index >= gsub.U16(list)) {
      continue;
    }
    const std::size_t before = found.ligatures.size();
    const std::size_t lookup = list + gsub.U16(list + 2 + (std::size_t{2} * index));
    const std::uint16_t type = gsub.U16(lookup);
    const std::uint16_t subtables = gsub.U16(lookup + 4);
    for (std::size_t s = 0; s < subtables && !gsub.Damaged(); ++s) {
      std::size_t sub = lookup + gsub.U16(lookup + 6 + (2 * s));
      std::uint16_t kind = type;
      if (type == kExtension) {
        // An extension is a longer way to say where a subtable is.
        kind = gsub.U16(sub + 2);
        sub += gsub.U32(sub + 4);
      }
      if (kind != kLigatureSubstitution || gsub.U16(sub) != 1) {
        ++found.subtablesLeftOut;
        continue;
      }
      // For each glyph it covers, in the order it covers them, a set of
      // the ligatures that begin with it.
      const std::vector<std::int32_t> places = Coverage(gsub, sub + gsub.U16(sub + 2));
      const std::uint16_t sets = gsub.U16(sub + 4);
      std::vector<std::uint16_t> firsts(sets, 0);
      std::vector<bool> covered(sets, false);
      for (const std::uint16_t glyph : glyphs) {
        const std::int32_t place = places[glyph];
        if (place >= 0 && place < sets) {
          firsts[static_cast<std::size_t>(place)] = glyph;
          covered[static_cast<std::size_t>(place)] = true;
        }
      }
      for (std::size_t place = 0; place < sets && !gsub.Damaged(); ++place) {
        if (!covered[place]) {
          continue;
        }
        const std::size_t set = sub + gsub.U16(sub + 6 + (2 * place));
        const std::uint16_t count = gsub.U16(set);
        for (std::size_t i = 0; i < count && !gsub.Damaged(); ++i) {
          const std::size_t ligature = set + gsub.U16(set + 2 + (2 * i));
          FontLigature one;
          one.glyph = gsub.U16(ligature);
          // It says how many glyphs it is made of, the first among them,
          // and names those after the first.
          const std::uint16_t made = gsub.U16(ligature + 2);
          one.letters.push_back(firsts[place]);
          bool all = made >= 2;
          for (std::size_t letter = 1; letter < made && !gsub.Damaged(); ++letter) {
            const std::uint16_t glyph = gsub.U16(ligature + 2 + (2 * letter));
            all = all && asked[glyph];
            one.letters.push_back(glyph);
          }
          const bool already = std::any_of(found.ligatures.begin(), found.ligatures.end(),
                                           [&](const FontLigature& other) { return other.letters == one.letters; });
          if (!all || already || gsub.Damaged()) {
            continue;
          }
          // The font goes through a line with the earlier lookups first. A
          // ligature made of a glyph one of them makes is made in two
          // steps, which a list cannot say; and one that a ligature of
          // theirs lies within is never made, those glyphs being joined
          // by the time the font comes to it.
          bool atOdds = false;
          for (const std::uint16_t letter : one.letters) {
            atOdds = atOdds || madeBefore[letter];
          }
          for (std::size_t earlier = 0; earlier < before && !atOdds; ++earlier) {
            const std::vector<std::uint16_t>& within = found.ligatures[earlier].letters;
            atOdds =
                std::search(one.letters.begin(), one.letters.end(), within.begin(), within.end()) != one.letters.end();
          }
          if (atOdds) {
            ++found.ligaturesLeftOut;
            continue;
          }
          found.ligatures.push_back(std::move(one));
        }
      }
    }
    for (std::size_t i = before; i < found.ligatures.size(); ++i) {
      madeBefore[found.ligatures[i].glyph] = true;
    }
  }
  return found;
}

namespace {

// The most glyphs a ligature an image holds is made of, and the most
// ligatures a face holds.
constexpr std::size_t kMostLetters = 3;
constexpr std::size_t kMostLigatures = 255;

}  // namespace

std::vector<std::uint16_t> LigatureGlyphs(const FontLigatures& font) {
  std::vector<std::uint16_t> glyphs;
  for (const FontLigature& ligature : font.ligatures) {
    if (ligature.letters.size() >= 2 && ligature.letters.size() <= kMostLetters &&
        std::find(glyphs.begin(), glyphs.end(), ligature.glyph) == glyphs.end()) {
      glyphs.push_back(ligature.glyph);
    }
  }
  return glyphs;
}

FaceLigatures FaceLigaturesOf(const FontLigatures& font, std::span<const std::uint32_t> drawnFrom) {
  FaceLigatures face;
  face.leftOut = font.subtablesLeftOut + font.ligaturesLeftOut;
  // The glyphs of the face that each of the font's glyphs is drawn as, of
  // those whose numbers a ligature's record holds.
  std::map<std::uint32_t, std::vector<std::uint16_t>> drawnAs;
  for (std::size_t glyph = 1; glyph < drawnFrom.size() && glyph <= 0xFFFFU; ++glyph) {
    drawnAs[drawnFrom[glyph]].push_back(static_cast<std::uint16_t>(glyph));
  }
  const std::vector<std::uint16_t> none;
  const auto as = [&](std::uint16_t fontGlyph) -> const std::vector<std::uint16_t>& {
    const auto found = drawnAs.find(fontGlyph);
    return found == drawnAs.end() ? none : found->second;
  };
  for (const FontLigature& ligature : font.ligatures) {
    const std::size_t letters = ligature.letters.size();
    const std::vector<std::uint16_t>& stands = as(ligature.glyph);
    if (letters < 2 || letters > kMostLetters || stands.empty()) {
      ++face.leftOut;
      continue;
    }
    // One for every two or three of the face's glyphs that are the font's.
    const std::vector<std::uint16_t> noThird = {0};
    const std::vector<std::uint16_t>& thirds = letters == 3 ? as(ligature.letters[2]) : noThird;
    bool made = false;
    for (const std::uint16_t first : as(ligature.letters[0])) {
      for (const std::uint16_t second : as(ligature.letters[1])) {
        for (const std::uint16_t third : thirds) {
          if (face.ligatures.size() < kMostLigatures) {
            face.ligatures.push_back({.first = first, .second = second, .third = third, .glyph = stands.front()});
            made = true;
          }
        }
      }
    }
    face.leftOut += made ? 0U : 1U;
  }
  return face;
}

}  // namespace bn::font_tool
