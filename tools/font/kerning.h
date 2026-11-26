#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A font's kerning, read out of the font's own tables, and put in the form
// a fonts image keeps it in (draw/font.h).
//
// The fonts the presets name keep their kerning in the table called GPOS,
// as the pair adjustments of the feature called `kern`: for two glyphs that
// follow one another, how much more or less than its advance the first
// moves the pen. FreeType, which draws the glyphs, reads only a part of
// that, so it is read here. What is read:
//
//   - The lookups of `kern` for the default language of every script the
//     font names, taken together, in the order the font has them. The fonts
//     of the presets give every script the same ones, or a script's own for
//     its own letters; so a pair is kerned as its script kerns it, and two
//     letters of different scripts as the font's tables have it.
//   - Of those lookups, the ones that are pair adjustments (type 2, or type
//     9 holding one). A lookup's subtables are tried in order: a list of
//     pairs (format 1) says nothing of a pair it does not name, and the
//     next subtable is tried; a table of classes (format 2) answers for
//     every pair whose first glyph it covers, and ends the lookup for them.
//     What several lookups say of one pair is added up.
//   - A lookup that passes over marks, base glyphs or ligatures (which
//     GDEF's classes of glyphs tell apart) kerns no pair with one of them
//     in it. In running text such a lookup would kern two letters across a
//     mark that stands between them; a table of pairs, asked about
//     neighbours, cannot.
//
// What is left out, and counted: lookups of `kern` that are not pair
// adjustments, which look at more than two glyphs; and whatever a pair
// adjusts besides the first glyph's advance.
//
// The tables are a font's, and are not trusted to be whole: a number that
// would be read from outside a table is read as nothing, and said.

namespace bn::font_tool {

// The second of two glyphs is set `units` of the font's units further on
// when it follows the first; less than nothing brings them together.
struct KernedPair {
  std::uint16_t first = 0;  // The font's own numbers for its glyphs.
  std::uint16_t second = 0;
  std::int32_t units = 0;

  bool operator==(const KernedPair&) const = default;
};

struct FontKerning {
  // Every pair of the glyphs asked about that is kerned, in order of the
  // first glyph and then the second.
  std::vector<KernedPair> pairs;
  // Lookups of `kern` that are not pair adjustments, and were left out.
  std::size_t lookupsLeftOut = 0;
  // Pairs that adjust something besides the first glyph's advance, which
  // was left out.
  std::size_t otherAdjustments = 0;
  // Something was to be read from outside a table.
  bool damaged = false;
};

// Reads the kerning of the glyphs `glyphs`, which are in order and each
// there once, from a font's GPOS table and its GDEF table. Either may be
// empty: a font with no GPOS has no kerning here, and one with no GDEF has
// no marks to pass over.
FontKerning ReadKerning(std::span<const std::uint8_t> gpos, std::span<const std::uint8_t> gdef,
                        std::span<const std::uint16_t> glyphs);

// One pair of a face's glyphs, by their numbers in the face, and how much
// further on the second is set, in quarters of a pixel.
struct FacePair {
  std::uint32_t first = 0;
  std::uint32_t second = 0;
  std::int32_t quarters = 0;
};

// A font's kerning as one face's. `drawnFrom` has, for each glyph of the
// face, the font's number for the glyph it is drawn from; the face is at
// `size` pixels to the em, and the font has `unitsToTheEm` units to it.
// Each of the font's pairs is put to the nearest quarter of a pixel, a half
// going away from nothing, and one that comes to nothing there is left out;
// and it is a pair of every two glyphs of the face that are drawn from the
// font's two, since a font may draw several characters with one glyph.
// Glyph 0 of a face, which is drawn for what the face lacks, is in no pair.
// None at all with no units to the em.
std::vector<FacePair> FacePairsOf(const FontKerning& kerning, std::span<const std::uint32_t> drawnFrom,
                                  std::uint16_t size, std::uint32_t unitsToTheEm);

// A face's kerning as an image keeps it: a class for each glyph as the
// first of two and one as the second, 0 for a glyph that does not kern,
// and a table of `firsts` rows of `seconds` signed bytes.
struct KerningClasses {
  // False if there are more classes of either kind than a byte counts
  // (255): the face cannot be kerned, and has no classes and no table here.
  bool fits = true;
  std::size_t firsts = 0;  // How many classes, the class of no kerning left out.
  std::size_t seconds = 0;
  std::vector<std::uint8_t> firstOf;  // For each glyph of the face.
  std::vector<std::uint8_t> secondOf;
  std::vector<std::int8_t> table;
  std::size_t pairs = 0;    // How many pairs are kerned.
  std::size_t clipped = 0;  // Pairs kerned by more than a signed byte holds, and held at that.
};

// Puts the glyphs of a face of `glyphs` glyphs in classes: two glyphs are
// of one class as firsts if they kern alike with every glyph after them,
// and of one as seconds if alike after every glyph before them, so no pair
// is kerned otherwise than `pairs` says. Classes are numbered from 1 in the
// order of the first glyph of each. A pair named twice is kerned as it is
// named last, and one of nothing is no pair.
KerningClasses MakeKerningClasses(std::size_t glyphs, std::span<const FacePair> pairs);

}  // namespace bn::font_tool
