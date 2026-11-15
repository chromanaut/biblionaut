#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Makes the table that layout/hyphenator.h reads, from a language's
// hyphenation patterns as they are written for TeX.

namespace bn::hyphen_tool {

struct PackOptions {
  // The fewest letters a word keeps before a break, and after one. They go
  // into the table as the language's own.
  std::uint8_t leftMin = 2;
  std::uint8_t rightMin = 3;
};

struct Packed {
  bool ok = false;
  std::string error;  // If not: what is wrong, with the line it is on.
  std::vector<std::uint8_t> table;

  // What went into it.
  std::size_t patterns = 0;
  std::size_t exceptions = 0;
  std::size_t letters = 0;  // The language's letters,
  std::size_t listed = 0;   // and the characters that stand for them, capitals among them.
  std::size_t states = 0;
  std::size_t edges = 0;
  std::size_t valueBytes = 0;
  std::size_t deepest = 0;  // The most letters in a pattern, a word's ends counted.
  std::size_t widest = 0;   // The most edges that leave one state.
  // What the edges would have been had like branches not been kept once.
  std::size_t edgesUnshared = 0;
};

// Packs a language.
//
// All three are UTF-8 text in which anything from a `%` to the end of its
// line is passed over.
//
//   - `patterns`: the patterns, with spaces or line ends between them. A
//     pattern is letters with a digit, 0 to 9, wherever it has a number, and
//     may begin or end with a `.` for the end of a word: `hy3ph`, `.un1`,
//     `4m1p`.
//   - `exceptions`: whole words written out with a `-` at each place they
//     may be broken, which no pattern is then let change: `ta-ble`,
//     `present`. May be empty.
//   - `letters`: a line for each letter that has other forms. The first
//     character of the line is the letter as the patterns write it, and the
//     rest are characters that are to count as it: `aA`, `éÉ`. May be empty,
//     and then the small letters a to z have their capitals and no other
//     letter has any.
Packed Pack(std::string_view patterns, std::string_view exceptions, std::string_view letters,
            const PackOptions& options);

}  // namespace bn::hyphen_tool
