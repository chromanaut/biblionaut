#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "layout/hyphenator.h"

// For tests of hyphenation. Patterns are written as TeX writes them, in the
// small letters a to z: "hy3ph", ".un1", "4m1p". Here are the places they
// give a word, found by laying every pattern over every place it could go;
// a table of them written the plainest way there is, with nothing shared;
// and a word broken by a table, shown with its breaks.

namespace bn::test {

// A pattern taken apart: its letters, with a `.` for an end of a word, and
// the number at each place, before each letter and after the last.
struct HyphenPattern {
  std::string letters;
  std::vector<std::uint8_t> numbers;
};

inline HyphenPattern ParseHyphenPattern(std::string_view text) {
  HyphenPattern pattern;
  pattern.numbers.push_back(0);
  for (const char ch : text) {
    if (ch >= '0' && ch <= '9') {
      pattern.numbers.back() = static_cast<std::uint8_t>(ch - '0');
    } else {
      pattern.letters += ch;
      pattern.numbers.push_back(0);
    }
  }
  return pattern;
}

// Where the patterns let `word` be broken: how many letters come before
// each break. `word` is small letters and nothing else.
inline std::vector<std::size_t> PlacesByHand(const std::vector<std::string>& patterns, std::string_view word,
                                             std::size_t leftMin, std::size_t rightMin) {
  const std::string marked = "." + std::string(word) + ".";
  std::vector<std::uint8_t> levels(marked.size() + 1, 0);
  for (const std::string& text : patterns) {
    const HyphenPattern pattern = ParseHyphenPattern(text);
    for (std::size_t at = 0; at + pattern.letters.size() <= marked.size(); ++at) {
      if (marked.compare(at, pattern.letters.size(), pattern.letters) != 0) {
        continue;
      }
      for (std::size_t place = 0; place < pattern.numbers.size(); ++place) {
        levels[at + place] = std::max(levels[at + place], pattern.numbers[place]);
      }
    }
  }
  std::vector<std::size_t> places;
  for (std::size_t before = leftMin; before + rightMin <= word.size(); ++before) {
    if (levels[before + 1] % 2 == 1) {
      places.push_back(before);
    }
  }
  return places;
}

// Another character that is to count as one of the letters a to z.
struct OtherForm {
  std::uint32_t codePoint = 0;
  char letter = 'a';
};

// A table of the patterns, written the plain way: the tree as it stands,
// a state for every branching, nothing kept once that occurs twice. The
// letters a to z are the letters 1 to 26, capital or small.
inline std::vector<std::uint8_t> PlainHyphenTable(const std::vector<std::string>& patterns, std::uint8_t leftMin,
                                                  std::uint8_t rightMin, const std::vector<OtherForm>& others = {}) {
  struct Node {
    std::map<std::uint8_t, std::size_t> children;
    std::vector<std::uint8_t> value;  // As it goes into the table; empty if no pattern ends here.
  };
  const auto symbolOf = [](char letter) {
    return letter == '.' ? std::uint8_t{0} : static_cast<std::uint8_t>(letter - 'a' + 1);
  };
  std::vector<Node> nodes(1);
  std::size_t deepest = 1;
  for (const std::string& text : patterns) {
    const HyphenPattern pattern = ParseHyphenPattern(text);
    std::size_t node = 0;
    for (const char letter : pattern.letters) {
      const std::uint8_t symbol = symbolOf(letter);
      if (!nodes[node].children.contains(symbol)) {
        nodes[node].children[symbol] = nodes.size();
        nodes.emplace_back();
      }
      node = nodes[node].children[symbol];
    }
    // The numbers that are not nothing, each with how far back from the
    // pattern's end its place is. If the pattern was there already, the
    // larger number at each place.
    std::array<std::uint8_t, 256> byBack{};
    for (std::size_t i = 1; i < nodes[node].value.size(); i += 2) {
      byBack[nodes[node].value[i]] = nodes[node].value[i + 1];
    }
    for (std::size_t place = 0; place < pattern.numbers.size(); ++place) {
      const std::size_t back = pattern.letters.size() - place;
      byBack[back] = std::max(byBack[back], pattern.numbers[place]);
    }
    std::vector<std::uint8_t> value = {0};
    for (std::size_t back = 0; back <= pattern.letters.size(); ++back) {
      if (byBack[back] != 0) {
        value.push_back(static_cast<std::uint8_t>(back));
        value.push_back(byBack[back]);
        ++value[0];
      }
    }
    nodes[node].value = value[0] != 0 ? value : std::vector<std::uint8_t>{};
    deepest = std::max(deepest, pattern.letters.size());
  }

  // Each node that branches is a state. Where each begins, the first node's
  // first.
  std::vector<std::size_t> startOf(nodes.size(), 0xFFFF);
  std::vector<std::size_t> order;
  std::size_t edgeCount = 0;
  std::size_t widest = 1;
  if (!nodes[0].children.empty()) {
    order.push_back(0);
    startOf[0] = 0;
    edgeCount = nodes[0].children.size();
  }
  for (std::size_t at = 0; at < order.size(); ++at) {
    const std::size_t node = order[at];
    widest = std::max(widest, nodes[node].children.size());
    for (const auto& [symbol, child] : nodes[node].children) {
      if (!nodes[child].children.empty()) {
        startOf[child] = edgeCount;
        edgeCount += nodes[child].children.size();
        order.push_back(child);
      }
    }
  }
  std::vector<std::uint8_t> edges;
  std::vector<std::uint8_t> values;
  for (const std::size_t node : order) {
    std::size_t left = nodes[node].children.size();
    for (const auto& [symbol, child] : nodes[node].children) {
      --left;
      std::size_t value = 0;
      if (!nodes[child].value.empty()) {
        value = values.size() + 1;
        values.insert(values.end(), nodes[child].value.begin(), nodes[child].value.end());
      }
      value |= left == 0 ? 0x8000U : 0U;
      edges.push_back(symbol);
      edges.push_back(static_cast<std::uint8_t>(startOf[child] & 0xFFU));
      edges.push_back(static_cast<std::uint8_t>(startOf[child] >> 8U));
      edges.push_back(static_cast<std::uint8_t>(value & 0xFFU));
      edges.push_back(static_cast<std::uint8_t>(value >> 8U));
    }
  }

  // The characters that are letters, in the order of their code points.
  std::map<std::uint32_t, std::uint8_t> listed;
  for (char letter = 'a'; letter <= 'z'; ++letter) {
    listed[static_cast<std::uint32_t>(letter)] = symbolOf(letter);
    listed[static_cast<std::uint32_t>(letter - 'a' + 'A')] = symbolOf(letter);
  }
  for (const OtherForm& other : others) {
    listed[other.codePoint] = symbolOf(other.letter);
  }

  std::vector<std::uint8_t> table = {'B', 'N', 'H', 'Y', 1, leftMin, rightMin, static_cast<std::uint8_t>(deepest)};
  const auto put16 = [&](std::size_t value) {
    table.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    table.push_back(static_cast<std::uint8_t>(value >> 8U));
  };
  put16(listed.size());
  put16(edgeCount);
  put16(values.size());
  table.push_back(static_cast<std::uint8_t>(widest));
  table.push_back(0);
  for (const auto& [codePoint, symbol] : listed) {
    table.push_back(static_cast<std::uint8_t>(codePoint & 0xFFU));
    table.push_back(static_cast<std::uint8_t>((codePoint >> 8U) & 0xFFU));
    table.push_back(static_cast<std::uint8_t>((codePoint >> 16U) & 0xFFU));
    table.push_back(symbol);
  }
  table.insert(table.end(), edges.begin(), edges.end());
  table.insert(table.end(), values.begin(), values.end());
  return table;
}

// Where an open table lets `word` be broken: how many bytes of the word
// come before each break.
inline std::vector<std::size_t> PlacesByTable(layout::Hyphenator& hyphenator, std::string_view word) {
  std::array<std::uint8_t, layout::Hyphenator::kMaxLetters> places{};
  const std::size_t count = hyphenator.Find(word, places);
  return {places.begin(), places.begin() + static_cast<std::ptrdiff_t>(count)};
}

// A word with a `-` at each of those places.
inline std::string ShownBroken(std::string_view word, const std::vector<std::size_t>& places) {
  std::string shown;
  std::size_t next = 0;
  for (std::size_t at = 0; at < word.size(); ++at) {
    if (next < places.size() && places[next] == at) {
      shown += '-';
      ++next;
    }
    shown += word[at];
  }
  return shown;
}

// A word as a table breaks it, shown with its breaks. The table has to be
// one that opens.
inline std::string BrokenByTable(std::span<const std::uint8_t> table, std::string_view word) {
  layout::Hyphenator hyphenator;
  if (hyphenator.Open(table) != layout::HyphenStatus::kOk) {
    return "(not a table)";
  }
  return ShownBroken(word, PlacesByTable(hyphenator, word));
}

}  // namespace bn::test
