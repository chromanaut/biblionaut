#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The ligatures a font makes of its own accord, read out of the font's own
// table, for the font builder to put in a fonts image (draw/font.h).
//
// A font says in the table called GSUB which glyphs it puts in place of
// which, and under the feature called `liga` those it joins in ordinary
// text: one glyph for an f and an i that follow one another, and the like.
// FreeType, which draws the glyphs, reads none of that, so it is read here.
// What is read:
//
//   - The lookups of `liga` for the default language of every script the
//     font names, taken together, in the order the font has them.
//   - Of those, the subtables that are ligature substitutions (type 4, or
//     type 7 holding one): for each glyph a ligature may begin with, the
//     ligatures that begin with it, each the glyphs that follow and the
//     glyph that stands for them all.
//   - They are kept in the font's own order, which is the order they are
//     tried in: for an f, its ligature with an f and an i before the one
//     with an f alone.
//
// Which fonts join which letters differs, and a font that has a glyph for a
// ligature need not make it: so nothing here knows of any particular
// letters.
//
// A font goes through a line with one lookup, and then through what that
// made of it with the next; a list is tried once at each place in turn,
// which is what one lookup does. So where a font has several lookups of
// ligatures, one of a later lookup is left out if a list cannot say it as
// the font makes it: if it is made of a glyph an earlier lookup makes,
// which is a ligature made in two steps; or if a ligature of an earlier
// lookup lies within it, since the font has joined those glyphs by the
// time it comes to this one, and never makes it. One difference is left:
// where a ligature of an earlier lookup begins inside one of a later
// lookup and runs on past its end, the font makes the earlier lookup's
// and a list the one that begins first in the line. A font that joins
// two f's and an i in one lookup, and two f's in the next, makes of
// three f's and an i an f and its ligature of three; a list joins the
// first two f's.
//
// What is left out, and counted: subtables of `liga` that are not ligature
// substitutions, and those ligatures of later lookups. A lookup that
// passes over marks is read as one that does not: letters with a mark
// between them are not joined.
//
// The table is a font's, and is not trusted to be whole: a number that
// would be read from outside it is read as nothing, and said.

namespace bn::font_tool {

// One ligature: the glyphs it is made of, two or more, as they follow one
// another, and the glyph put in their place. The font's own numbers.
struct FontLigature {
  std::vector<std::uint16_t> letters;
  std::uint16_t glyph = 0;

  bool operator==(const FontLigature&) const = default;
};

struct FontLigatures {
  // Every ligature made of the glyphs asked about, in the order the font
  // tries them. One made of the same glyphs as an earlier one is left out:
  // it would never be come to.
  std::vector<FontLigature> ligatures;
  // Subtables of `liga` that are not ligature substitutions.
  std::size_t subtablesLeftOut = 0;
  // Ligatures of a lookup after the first that a list cannot say as the
  // font makes them.
  std::size_t ligaturesLeftOut = 0;
  // Something was to be read from outside the table.
  bool damaged = false;
};

// Reads the ligatures a font makes of the glyphs `glyphs`, which are in
// order and each there once, from its GSUB table. A ligature is read if
// every glyph it is made of is among them; the glyph that stands for them
// may be any. A font with no GSUB has none.
FontLigatures ReadLigatures(std::span<const std::uint8_t> gsub, std::span<const std::uint16_t> glyphs);

// The glyphs that stand for the ligatures of `font` that an image can hold,
// which are those of two glyphs or three: each once, in the order they are
// first come to. The builder draws those that no character has.
std::vector<std::uint16_t> LigatureGlyphs(const FontLigatures& font);

// One ligature of a face, by the face's own numbers for its glyphs: the
// two or three it is made of, `third` being 0 for one of two, and the glyph
// that stands for them.
struct FaceLigature {
  std::uint16_t first = 0;
  std::uint16_t second = 0;
  std::uint16_t third = 0;
  std::uint16_t glyph = 0;

  bool operator==(const FaceLigature&) const = default;
};

struct FaceLigatures {
  // In the order they are to be tried, which is the font's.
  std::vector<FaceLigature> ligatures;
  // Ligatures of the font that the face has not: of more than three
  // glyphs, with a glyph the face has not drawn or numbers above 65,535,
  // or past the 255 a face holds; and with them what was left out in
  // reading the font, its subtables and its ligatures.
  std::size_t leftOut = 0;
};

// A font's ligatures as one face's. `drawnFrom` has, for each glyph of the
// face, the font's number for the glyph it is drawn from. A ligature is one
// of every two or three glyphs of the face that are drawn from the font's,
// since a font may draw several characters with one glyph; and the glyph
// that stands for them is the first of the face's that is drawn from the
// font's. Glyph 0 of a face, which is drawn for what the face lacks, is in
// no ligature.
FaceLigatures FaceLigaturesOf(const FontLigatures& font, std::span<const std::uint32_t> drawnFrom);

}  // namespace bn::font_tool
