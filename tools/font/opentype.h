#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

// What the reading of a font's kerning (font/kerning.h) and of its
// ligatures (font/ligatures.h) share: the two tables they are in, GPOS and
// GSUB, are laid out alike as far as their scripts, features and lookups
// go, and name glyphs in the same two ways.
//
// A table is a font's, and is not trusted to be whole.

namespace bn::font_tool {

// A font numbers its glyphs in 16 bits.
inline constexpr std::size_t kGlyphNumbers = 65536;

// A font's table, whose numbers are big-endian. A number that would be read
// from outside it is nothing, and `damaged` is then set.
class Table {
 public:
  Table(std::span<const std::uint8_t> bytes, bool& damaged) : bytes_(bytes), damaged_(&damaged) {}

  std::uint16_t U16(std::size_t at) const {
    if (at > bytes_.size() || bytes_.size() - at < 2) {
      *damaged_ = true;
      return 0;
    }
    return static_cast<std::uint16_t>((std::uint32_t{bytes_[at]} << 8U) | bytes_[at + 1]);
  }
  std::int16_t S16(std::size_t at) const { return static_cast<std::int16_t>(U16(at)); }
  std::uint32_t U32(std::size_t at) const { return (std::uint32_t{U16(at)} << 16U) | U16(at + 2); }
  bool Damaged() const { return *damaged_; }
  bool Empty() const { return bytes_.empty(); }

 private:
  std::span<const std::uint8_t> bytes_;
  bool* damaged_;
};

// A coverage table at `at`: for each glyph, its place among the glyphs
// covered, or -1 for one that is not.
std::vector<std::int32_t> Coverage(const Table& table, std::size_t at);

// A table of classes at `at`: the class of each glyph, 0 for one it does
// not name.
std::vector<std::uint16_t> Classes(const Table& table, std::size_t at);

// The numbers of the lookups of the feature `tag` (its four letters, the
// first highest) for the default language of every script a GPOS or GSUB
// table names, taken together, in order.
std::set<std::uint16_t> FeatureLookups(const Table& table, std::uint32_t tag);

}  // namespace bn::font_tool
