#include "hyphen/pack.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "layout/hyphenator.h"

namespace bn::hyphen_tool {

namespace {

// What the table can hold: see layout/hyphenator.h.
constexpr std::size_t kMostLetters = 254;
constexpr std::size_t kMostEdges = 0xFFFF;
constexpr std::size_t kMostValueBytes = 0x7FFE;
constexpr std::uint32_t kNoNext = 0xFFFF;
constexpr std::uint32_t kLastEdge = 0x8000;
// The numbers an exception is written with. They are higher than any a
// pattern can carry, so nothing a pattern says changes them: one even, for
// a place that is not to be broken, and one odd, for a place that may be.
constexpr std::uint8_t kNeverBreak = 14;
constexpr std::uint8_t kMayBreak = 15;

// A letter, as a table knows it: 0 for the end of a word.
using Symbol = std::uint8_t;
constexpr Symbol kWordEnd = 0;

// A pattern's numbers: (how many letters back from its end, the number),
// for the places where it has one, nearest the end first.
using Value = std::vector<std::pair<std::uint8_t, std::uint8_t>>;

// One token of the text, as code points, and the line it was on.
struct Token {
  std::vector<std::uint32_t> characters;
  std::size_t line = 0;
};

std::string OnLine(std::size_t line, const std::string& what) { return "line " + std::to_string(line) + ": " + what; }

// Reads the character at `text[at]`, strictly. False if the bytes there are
// not one.
bool CharacterAt(std::string_view text, std::size_t at, std::uint32_t& codePoint, std::size_t& bytes) {
  const auto lead = static_cast<std::uint8_t>(text[at]);
  if (lead < 0x80) {
    codePoint = lead;
    bytes = 1;
    return true;
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t least = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    value = lead & 0x1FU;
    least = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    value = lead & 0x0FU;
    least = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    value = lead & 0x07U;
    least = 0x10000;
  } else {
    return false;
  }
  if (text.size() - at < length) {
    return false;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const auto next = static_cast<std::uint8_t>(text[at + i]);
    if ((next & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (next & 0x3FU);
  }
  if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    return false;
  }
  codePoint = value;
  bytes = length;
  return true;
}

bool IsSpace(std::uint32_t character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f';
}

// Cuts text into its tokens: runs of characters with no space in them,
// leaving out whatever follows a `%` on its line. If `byLine`, a token is a
// whole line with its spaces taken out.
bool Tokens(std::string_view text, bool byLine, std::vector<Token>& tokens, std::string& error) {
  Token token;
  std::size_t line = 1;
  bool inComment = false;
  const auto end = [&] {
    if (!token.characters.empty()) {
      tokens.push_back(token);
      token.characters.clear();
    }
  };
  for (std::size_t at = 0; at < text.size();) {
    std::uint32_t character = 0;
    std::size_t bytes = 0;
    if (!CharacterAt(text, at, character, bytes)) {
      error = OnLine(line, "not UTF-8");
      return false;
    }
    at += bytes;
    if (character == '\n') {
      end();
      ++line;
      inComment = false;
    } else if (inComment) {
      // Passed over.
    } else if (character == '%') {
      inComment = true;
    } else if (IsSpace(character)) {
      if (!byLine) {
        end();
      }
    } else {
      if (token.characters.empty()) {
        token.line = line;
      }
      token.characters.push_back(character);
    }
  }
  end();
  return true;
}

// A pattern as it goes into the tree: its letters, with the ends of a word
// among them, and the number at each place: before each letter, and after
// the last.
struct Pattern {
  std::vector<std::uint32_t> letters;  // 0 for the end of a word.
  std::vector<std::uint8_t> numbers;
};

bool ReadPattern(const Token& token, Pattern& pattern, std::string& error) {
  pattern = Pattern{};
  pattern.numbers.push_back(0);
  bool numbered = false;  // The place being stood at has its number.
  for (std::size_t i = 0; i < token.characters.size(); ++i) {
    const std::uint32_t character = token.characters[i];
    if (character >= '0' && character <= '9') {
      if (numbered) {
        error = OnLine(token.line, "two numbers at one place in a pattern");
        return false;
      }
      pattern.numbers.back() = static_cast<std::uint8_t>(character - '0');
      numbered = true;
      continue;
    }
    if (character == '.') {
      const bool first = pattern.letters.empty() && !numbered;
      const bool last = i + 1 == token.characters.size();
      if (!first && !last) {
        error = OnLine(token.line, "a `.` that is not at an end of its pattern");
        return false;
      }
      pattern.letters.push_back(0);
    } else {
      pattern.letters.push_back(character);
    }
    pattern.numbers.push_back(0);
    numbered = false;
  }
  if (std::none_of(pattern.letters.begin(), pattern.letters.end(), [](std::uint32_t letter) { return letter != 0; })) {
    error = OnLine(token.line, "a pattern with no letters");
    return false;
  }
  return true;
}

// A word written out with its breaks, as the pattern that says the same of
// that word and of no other: the whole word between its two ends, with a
// number at every place between two letters.
bool ReadException(const Token& token, Pattern& pattern, std::string& error) {
  pattern = Pattern{};
  pattern.letters.push_back(0);
  pattern.numbers = {0, 0};
  for (const std::uint32_t character : token.characters) {
    if (character == '-') {
      if (pattern.letters.size() < 2 || pattern.numbers.back() == kMayBreak) {
        error = OnLine(token.line, "a `-` with no letter before it");
        return false;
      }
      pattern.numbers.back() = kMayBreak;
      continue;
    }
    if (character == '.' || (character >= '0' && character <= '9')) {
      error = OnLine(token.line, "an exception is a word, with `-` where it may be broken");
      return false;
    }
    pattern.letters.push_back(character);
    pattern.numbers.push_back(kNeverBreak);
  }
  if (pattern.letters.size() < 2) {
    error = OnLine(token.line, "an exception with no letters");
    return false;
  }
  if (pattern.numbers.back() == kMayBreak) {
    error = OnLine(token.line, "a `-` with no letter after it");
    return false;
  }
  // The places beside the word's ends are not between two letters.
  pattern.numbers[1] = 0;
  pattern.numbers.back() = 0;
  pattern.letters.push_back(0);
  pattern.numbers.push_back(0);
  return true;
}

// The tree of patterns, a letter to a branch.
struct Node {
  std::map<Symbol, std::size_t> children;
  Value value;
};

// What leaves a state, an edge for each letter in order: the letter, where
// its pattern's numbers are (0 if no pattern ends there), and the state it
// leads to (kNoNext if none).
using Edge = std::tuple<Symbol, std::uint32_t, std::uint32_t>;
using State = std::vector<Edge>;

class Packer {
 public:
  explicit Packer(Packed& packed) : packed_(packed) { nodes_.emplace_back(); }

  void Add(const std::vector<Symbol>& symbols, const std::vector<std::uint8_t>& numbers) {
    std::size_t node = 0;
    for (const Symbol symbol : symbols) {
      const auto found = nodes_[node].children.find(symbol);
      if (found != nodes_[node].children.end()) {
        node = found->second;
      } else {
        nodes_.emplace_back();
        nodes_[node].children[symbol] = nodes_.size() - 1;
        node = nodes_.size() - 1;
      }
    }
    // The same pattern twice says, at each place, the larger of what its
    // two writings say.
    std::array<std::uint8_t, 256> byBack{};
    for (const auto& [back, number] : nodes_[node].value) {
      byBack[back] = number;
    }
    for (std::size_t place = 0; place < numbers.size(); ++place) {
      const std::size_t back = symbols.size() - place;
      byBack[back] = std::max(byBack[back], numbers[place]);
    }
    nodes_[node].value.clear();
    for (std::size_t back = 0; back <= symbols.size(); ++back) {
      if (byBack[back] != 0) {
        nodes_[node].value.emplace_back(static_cast<std::uint8_t>(back), byBack[back]);
      }
    }
    packed_.deepest = std::max(packed_.deepest, symbols.size());
  }

  // Turns the tree into states, keeping one of any that are alike, and
  // writes them out. False, with the error set, if they do not fit a table.
  bool Write(std::vector<std::uint8_t>& edges, std::vector<std::uint8_t>& values) {
    const std::uint32_t root = StatesOfTheTree();
    if (!packed_.error.empty()) {
      return false;
    }
    packed_.edgesUnshared = nodes_.size() - 1;
    if (root == kNoNext) {
      packed_.widest = 1;
      values = values_;
      return true;
    }
    // Where each state's edges begin: the first state first, and the rest
    // as they are come to from it.
    std::vector<std::uint32_t> startOf(states_.size(), kNoNext);
    std::vector<std::uint32_t> order = {root};
    std::size_t total = states_[root].size();
    startOf[root] = 0;
    for (std::size_t at = 0; at < order.size(); ++at) {
      // By index, not by reference: `order` grows as this goes.
      const std::uint32_t state = order[at];
      for (const Edge& edge : states_[state]) {
        const std::uint32_t next = std::get<2>(edge);
        if (next != kNoNext && startOf[next] == kNoNext) {
          startOf[next] = static_cast<std::uint32_t>(total);
          total += states_[next].size();
          order.push_back(next);
        }
      }
    }
    if (total > kMostEdges) {
      packed_.error = "too many patterns: they need " + std::to_string(total) + " edges, and a table holds " +
                      std::to_string(kMostEdges);
      return false;
    }
    for (const std::uint32_t state : order) {
      const State& leaving = states_[state];
      packed_.widest = std::max(packed_.widest, leaving.size());
      for (std::size_t n = 0; n < leaving.size(); ++n) {
        const auto& [symbol, value, next] = leaving[n];
        const std::uint32_t nextAt = next == kNoNext ? kNoNext : startOf[next];
        const std::uint32_t valueAndLast = value | (n + 1 == leaving.size() ? kLastEdge : 0U);
        edges.push_back(symbol);
        edges.push_back(static_cast<std::uint8_t>(nextAt & 0xFFU));
        edges.push_back(static_cast<std::uint8_t>(nextAt >> 8U));
        edges.push_back(static_cast<std::uint8_t>(valueAndLast & 0xFFU));
        edges.push_back(static_cast<std::uint8_t>(valueAndLast >> 8U));
      }
    }
    packed_.states = order.size();
    packed_.edges = total;
    values = values_;
    return true;
  }

 private:
  // Makes the state each node's branches come to, which is one made before
  // if there is one like it, and returns the first node's: kNoNext if the
  // tree has no branches at all. A node is always made after the node it
  // branches from, so going through them last to first comes to every
  // node's branches before the node itself.
  std::uint32_t StatesOfTheTree() {
    std::vector<std::uint32_t> stateOf(nodes_.size(), kNoNext);
    for (std::size_t node = nodes_.size(); node-- > 0;) {
      if (nodes_[node].children.empty()) {
        continue;
      }
      State state;
      state.reserve(nodes_[node].children.size());
      for (const auto& [symbol, child] : nodes_[node].children) {
        state.emplace_back(symbol, ValueOf(nodes_[child].value), stateOf[child]);
      }
      const auto [found, added] = stateIds_.try_emplace(state, static_cast<std::uint32_t>(states_.size()));
      if (added) {
        states_.push_back(state);
      }
      stateOf[node] = found->second;
    }
    return stateOf[0];
  }

  // Where a pattern's numbers are among the values, and one more than that;
  // 0 if it has none. Numbers that are alike are kept once.
  std::uint32_t ValueOf(const Value& value) {
    if (value.empty()) {
      return 0;
    }
    const auto [found, added] = valueIds_.try_emplace(value, static_cast<std::uint32_t>(values_.size() + 1));
    if (added) {
      values_.push_back(static_cast<std::uint8_t>(value.size()));
      for (const auto& [back, number] : value) {
        values_.push_back(back);
        values_.push_back(number);
      }
      if (values_.size() > kMostValueBytes && packed_.error.empty()) {
        packed_.error = "too many patterns: their numbers take more than " + std::to_string(kMostValueBytes) + " bytes";
      }
    }
    return found->second;
  }

  Packed& packed_;
  std::vector<Node> nodes_;
  std::map<State, std::uint32_t> stateIds_;
  std::vector<State> states_;
  std::map<Value, std::uint32_t> valueIds_;
  std::vector<std::uint8_t> values_;
};

void Put16(std::vector<std::uint8_t>& bytes, std::size_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

}  // namespace

Packed Pack(std::string_view patterns, std::string_view exceptions, std::string_view letters,
            const PackOptions& options) {
  Packed packed;
  if (options.leftMin == 0 || options.rightMin == 0) {
    packed.error = "a break needs a letter before it and a letter after it at the least";
    return packed;
  }

  // The patterns and the exceptions, both as patterns.
  std::vector<Pattern> all;
  std::vector<Token> tokens;
  if (!Tokens(patterns, false, tokens, packed.error)) {
    packed.error = "patterns, " + packed.error;
    return packed;
  }
  for (const Token& token : tokens) {
    Pattern pattern;
    if (!ReadPattern(token, pattern, packed.error)) {
      packed.error = "patterns, " + packed.error;
      return packed;
    }
    all.push_back(pattern);
  }
  packed.patterns = all.size();
  tokens.clear();
  if (!Tokens(exceptions, false, tokens, packed.error)) {
    packed.error = "exceptions, " + packed.error;
    return packed;
  }
  for (const Token& token : tokens) {
    Pattern pattern;
    if (!ReadException(token, pattern, packed.error)) {
      packed.error = "exceptions, " + packed.error;
      return packed;
    }
    all.push_back(pattern);
  }
  packed.exceptions = all.size() - packed.patterns;

  // The language's letters: every one a pattern uses, and every one the
  // list of letters gives. Each has a number, in the order of their code
  // points.
  tokens.clear();
  if (!Tokens(letters, true, tokens, packed.error)) {
    packed.error = "letters, " + packed.error;
    return packed;
  }
  std::map<std::uint32_t, Symbol> symbolOf;
  for (const Pattern& pattern : all) {
    for (const std::uint32_t letter : pattern.letters) {
      if (letter != 0) {
        symbolOf[letter] = 0;
      }
    }
  }
  for (const Token& token : tokens) {
    symbolOf[token.characters[0]] = 0;
  }
  if (symbolOf.size() > kMostLetters) {
    packed.error =
        "too many letters: " + std::to_string(symbolOf.size()) + ", and a table holds " + std::to_string(kMostLetters);
    return packed;
  }
  Symbol next = 1;
  for (auto& [letter, symbol] : symbolOf) {
    symbol = next;
    ++next;
  }
  packed.letters = symbolOf.size();

  // The characters that stand for them: each letter itself, and its other
  // forms.
  std::map<std::uint32_t, Symbol> listed = symbolOf;
  const auto alsoStandsFor = [&](std::uint32_t character, std::uint32_t letter, std::size_t line) {
    const auto [found, added] = listed.try_emplace(character, symbolOf[letter]);
    if (!added && found->second != symbolOf[letter]) {
      packed.error = "letters, " + OnLine(line, "a character that stands for two letters");
      return false;
    }
    return true;
  };
  if (tokens.empty()) {
    for (std::uint32_t small = 'a'; small <= 'z'; ++small) {
      if (symbolOf.contains(small) && !alsoStandsFor(small - 'a' + 'A', small, 0)) {
        return packed;
      }
    }
  }
  for (const Token& token : tokens) {
    for (std::size_t i = 1; i < token.characters.size(); ++i) {
      if (!alsoStandsFor(token.characters[i], token.characters[0], token.line)) {
        return packed;
      }
    }
  }
  if (listed.size() > layout::Hyphenator::kMaxListed) {
    packed.error = "too many characters stand for letters: " + std::to_string(listed.size()) + ", and a table holds " +
                   std::to_string(layout::Hyphenator::kMaxListed);
    return packed;
  }
  packed.listed = listed.size();

  // The tree, and the states it comes down to.
  Packer packer(packed);
  for (const Pattern& pattern : all) {
    if (pattern.letters.size() > layout::Hyphenator::kMaxDeepest) {
      packed.error = "a pattern of " + std::to_string(pattern.letters.size()) +
                     " letters, and the longest a table takes is " + std::to_string(layout::Hyphenator::kMaxDeepest);
      return packed;
    }
    std::vector<Symbol> symbols;
    symbols.reserve(pattern.letters.size());
    for (const std::uint32_t letter : pattern.letters) {
      symbols.push_back(letter == 0 ? kWordEnd : symbolOf[letter]);
    }
    packer.Add(symbols, pattern.numbers);
  }
  std::vector<std::uint8_t> edges;
  std::vector<std::uint8_t> values;
  if (!packer.Write(edges, values)) {
    return packed;
  }
  packed.deepest = std::max<std::size_t>(packed.deepest, 1);
  packed.valueBytes = values.size();

  // The table itself.
  std::vector<std::uint8_t>& table = packed.table;
  table = {'B', 'N', 'H', 'Y', 1, options.leftMin, options.rightMin, static_cast<std::uint8_t>(packed.deepest)};
  Put16(table, packed.listed);
  Put16(table, packed.edges);
  Put16(table, packed.valueBytes);
  table.push_back(static_cast<std::uint8_t>(packed.widest));
  table.push_back(0);
  for (const auto& [character, symbol] : listed) {
    table.push_back(static_cast<std::uint8_t>(character & 0xFFU));
    table.push_back(static_cast<std::uint8_t>((character >> 8U) & 0xFFU));
    table.push_back(static_cast<std::uint8_t>((character >> 16U) & 0xFFU));
    table.push_back(symbol);
  }
  table.insert(table.end(), edges.begin(), edges.end());
  table.insert(table.end(), values.begin(), values.end());
  packed.ok = true;
  return packed;
}

}  // namespace bn::hyphen_tool
