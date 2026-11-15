#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Finding where a word may be broken across two lines.
//
// The method is Liang's, the one TeX uses (Franklin Mark Liang, "Word
// Hy-phen-a-tion by Com-put-er", 1983). A language is a set of patterns:
// short runs of letters with a number at each place between them, such as
// hy3ph or 1tio. A word is set between two marks for its ends, and every
// pattern that occurs anywhere in it is laid over it. Each place between
// two letters takes the largest number any pattern puts there, and the word
// may be broken where that number is odd. So one pattern can allow a break
// and a longer one forbid it again.
//
// The patterns come to this code as a table, made from a language's
// patterns by the bn-hyphen tool and read in place, wherever its bytes are:
// nothing is copied out of it and nothing is allocated. The table is not
// trusted. Whatever its bytes are, a word is answered within a bound the
// table itself states (MostWorkFor()), and nothing outside the table is
// read.
//
// The table, whose numbers are little-endian:
//
//   header, 16 bytes:
//     "BNHY"
//     version         1
//     left            the fewest letters before a break
//     right           the fewest letters after one
//     deepest         the most letters in a pattern, a word's ends counted
//     letters   (2)   how many entries the letter list has
//     edges     (2)   how many edges there are
//     values    (2)   how many bytes of values there are
//     widest          the most edges that leave one state
//     0
//   the letter list, 4 bytes an entry, in order of code point:
//     code point (3)  a character that counts as a letter
//     letter          which letter: 1 to 254. Capitals are entries of their
//                     own that name the same letter as their small forms.
//   the edges, 5 bytes each. The patterns are a tree, read a letter at a
//   time from a pattern's first; branches that are alike are kept once. A
//   state is a run of edges, and the first state begins at edge 0:
//     letter          the letter this edge is taken on; 0 is a word's end
//     next      (2)   the edge the state it leads to begins at; 0xFFFF if
//                     no pattern goes on from here
//     value     (2)   bit 15: this is the last edge of its state. The rest:
//                     0 if no pattern ends with this edge; otherwise one
//                     more than where its numbers begin among the values
//   the values. A pattern's numbers, left out where they are nothing:
//     count           how many there are
//     count times:
//       back          how many letters back from the pattern's end the
//                     place is
//       number        1 to 15

namespace bn::layout {

// How opening a table has gone.
enum class HyphenStatus : std::uint8_t {
  kOk = 0,
  kNotOpen,   // Open() has not been called.
  kBadTable,  // The bytes are not a table.
};

// The name of a status, for diagnostics: "ok", "not open", ...
const char* HyphenStatusName(HyphenStatus status) noexcept;

// Finds the places a word may be broken, by one language's table.
//
// Used by one task.
class Hyphenator final {
 public:
  // The longest word it looks at, in characters. A longer one is given no
  // places.
  static constexpr std::size_t kMaxLetters = 63;
  // The most entries a table's letter list may have, and the most letters a
  // pattern may: a table that says more is refused.
  static constexpr std::size_t kMaxListed = 1024;
  static constexpr std::size_t kMaxDeepest = kMaxLetters + 2;

  Hyphenator() noexcept = default;

  // Takes `table` as the language's patterns. Its bytes are used where they
  // lie, and have to stay there for as long as words are asked about. Goes
  // over the letter list once, and nothing else of the table.
  [[nodiscard]] HyphenStatus Open(std::span<const std::uint8_t> table) noexcept;

  HyphenStatus Status() const noexcept { return status_; }

  // Finds where `word` may be broken. `word` is UTF-8: a run of characters
  // with no space in it. Each place is written to `places` as the number of
  // bytes of the word that come before the break, smallest first, and the
  // count of them is returned. kMaxLetters places are always room enough;
  // with room for fewer, the first ones are given.
  //
  // Capitals are taken as their small letters. A character the language has
  // no letter for divides the word: each side is looked at as a word of its
  // own, and no break is put next to it. The same goes for bytes that are
  // not UTF-8. Without a table there are no places.
  std::size_t Find(std::string_view word, std::span<std::uint8_t> places) noexcept;

  // The fewest letters this language leaves before a break, and after one.
  std::uint8_t LeftMin() const noexcept { return leftMin_; }
  std::uint8_t RightMin() const noexcept { return rightMin_; }

  // The most work Find() does for a word of `letters` characters, with the
  // table that is open: whatever the word, and whatever else the table
  // holds. Work is counted in edges looked at and numbers laid over the
  // word. A word is gone through from each of its letters, and from each
  // end of each stretch of letters; from each, no further than the longest
  // pattern; and at each step, over no more edges than leave one state, and
  // no more numbers than a pattern has places. Finding which letter a
  // character is takes ten steps at the most, and is not counted.
  std::uint32_t MostWorkFor(std::size_t letters) const noexcept;
  // The work the last Find() did, counted the same way.
  std::uint32_t LastWork() const noexcept { return work_; }

 private:
  // Looks at one stretch of letters as a word: symbols_[first] up to, and
  // not including, symbols_[end]. Returns the count of places with the ones
  // it adds.
  std::size_t FindIn(std::size_t first, std::size_t end, std::span<std::uint8_t> places, std::size_t count) noexcept;
  // Lays over `levels` the numbers of the pattern that ends `length`
  // symbols on from the place `from`, if they are where `value` says.
  void Apply(std::uint16_t value, std::size_t from, std::size_t length, std::span<std::uint8_t> levels) noexcept;
  std::uint8_t LetterOf(std::uint32_t codePoint) const noexcept;

  HyphenStatus status_ = HyphenStatus::kNotOpen;
  std::span<const std::uint8_t> listed_;  // The letter list,
  std::span<const std::uint8_t> edges_;   // the edges,
  std::span<const std::uint8_t> values_;  // and the values, each where it lies in the table.
  std::uint8_t leftMin_ = 0;
  std::uint8_t rightMin_ = 0;
  std::uint8_t deepest_ = 0;
  std::uint8_t widest_ = 0;
  std::uint32_t work_ = 0;

  // The word being looked at: which letter each character is, and how many
  // bytes of the word come before it.
  std::array<std::uint8_t, kMaxLetters> symbols_{};
  std::array<std::uint8_t, kMaxLetters + 1> offsets_{};
};

}  // namespace bn::layout
