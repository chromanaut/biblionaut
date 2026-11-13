#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

// Small things to do with the names and lists of words in a book's XML,
// for this part's own sources.

namespace bn::epub {

constexpr char AsciiLower(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

// Whether two names are the same but for capitals among the letters of
// English.
inline bool SameButForCase(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return AsciiLower(x) == AsciiLower(y); });
}

// Whether `word` is one of the words of `list`, which has spaces between
// them.
inline bool HasWord(std::string_view list, std::string_view word) noexcept {
  std::size_t start = 0;
  for (std::size_t i = 0; i <= list.size(); ++i) {
    if (i == list.size() || list[i] == ' ' || list[i] == '\t' || list[i] == '\n' || list[i] == '\r') {
      if (std::string_view(list.data() + start, i - start) == word) {
        return true;
      }
      start = i + 1;
    }
  }
  return false;
}

}  // namespace bn::epub
