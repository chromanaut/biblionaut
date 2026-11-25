// The fuzz target for the hyphenator (layout/hyphenator.h), which reads two
// things it does not trust: a language's table of patterns, and the words
// of a book.
//
// What it is given:
//
//   byte 0   bit 0:        how the language is given: as a table (0) or as
//                          patterns (1)
//            bits 1 to 3:  how many places there is room for, when a word
//                          is asked about a second time
//
//   then, for a table: the table as it stands. The first 16 bytes are taken
//   for its header, and after them as many bytes as that header says its
//   three pieces come to, or all there are if there are not so many.
//
//   for patterns: one byte, bits 0 to 2 one less than the fewest letters
//   before a break and bits 3 to 5 one less than the fewest after; then the
//   patterns as TeX writes them (hy3ph .un1 4m1p), a space between one and
//   the next, up to a line end. A byte that is no small letter, digit, full
//   stop or space is taken for a small letter. The target makes a table of
//   them the plain way, with nothing shared.
//
//   then the words, each up to a line end. With patterns, a byte of a word
//   that is no letter from a to z, capital or small, is taken for a small
//   one.
//
// What has to hold, whatever the bytes are:
//
//   - a table is opened or refused, and a refused one gives no word a
//     place; one that is opened is a header and its three pieces and
//     nothing more;
//   - nothing outside the table or the word is read, and nothing is written
//     outside the room for places: each is in memory of exactly its size,
//     where the sanitizer sees it;
//   - no word takes more work than MostWorkFor() says a word of as many
//     characters may;
//   - a word's places are in order, each at the start of one of its
//     characters; and by the target's own reading of the word and of the
//     table's letter list, at least LeftMin() letters come directly before
//     each and at least RightMin() directly after, so that none is next to
//     anything that is not a letter;
//   - a word of more than kMaxLetters characters has no places;
//   - with room for fewer places, the first ones are given;
//   - the same table somewhere else in memory, taken by a hyphenator that
//     had another table open before it, gives the same places;
//   - with a table, the places are the ones found by reading the table an
//     edge at a time, each stretch of the word's letters by itself;
//   - with patterns, the places are the ones found by laying every pattern
//     over every place in the word (both in tests/support/hyphen_patterns.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_support.h"
#include "hyphen_patterns.h"
#include "layout/hyphenator.h"

namespace {

using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;

// A table's pieces, in bytes, as layout/hyphenator.h lays them out.
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kListedBytes = 4;
constexpr std::size_t kEdgeBytes = 5;

constexpr std::uint8_t kLineEnd = '\n';
// The most words of one input that are asked about. A table can make one
// word cost a million steps, and an input is to be done with in a small
// part of a second.
constexpr std::size_t kMostWords = 16;
// How many places there is room for the second time a word is asked about.
constexpr std::array<std::size_t, 8> kRoom = {0, 1, 2, 3, 5, 8, 20, 62};
// Of patterns: the most bytes that are read, which keeps the table made of
// them within what its counts of two bytes hold; and the most letters one
// may have, which keeps it within what a table may say its longest has.
constexpr std::size_t kMostPatternBytes = 4096;
constexpr std::size_t kMostPatternLetters = 12;

std::uint32_t Number16(std::span<const std::uint8_t> bytes, std::size_t at) {
  return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8U);
}

// The next line of `bytes`, taken off the front of them: what comes before
// the first line end, or everything if there is none.
std::span<const std::uint8_t> TakeLine(std::span<const std::uint8_t>& bytes) {
  const auto end = std::find(bytes.begin(), bytes.end(), kLineEnd);
  const auto length = static_cast<std::size_t>(end - bytes.begin());
  const std::span<const std::uint8_t> line = bytes.first(length);
  bytes = bytes.subspan(std::min(length + 1, bytes.size()));
  return line;
}

// The letter list of a table that was opened.
std::span<const std::uint8_t> LetterListOf(std::span<const std::uint8_t> table) {
  return table.subspan(kHeaderBytes, Number16(table, 8) * kListedBytes);
}

// Which letter a letter list says `codePoint` is, and 0 if it names no such
// character: looked for from the front, an entry at a time.
std::uint8_t LetterListed(std::span<const std::uint8_t> listed, std::uint32_t codePoint) {
  for (std::size_t at = 0; at + kListedBytes <= listed.size(); at += kListedBytes) {
    if ((Number16(listed, at) | (std::uint32_t{listed[at + 2]} << 16U)) == codePoint) {
      return listed[at + 3];
    }
  }
  return 0;
}

// How many bytes UTF-8 writes a code point in.
std::size_t BytesFor(std::uint32_t codePoint) {
  if (codePoint < 0x80) {
    return 1;
  }
  if (codePoint < 0x800) {
    return 2;
  }
  return codePoint < 0x10000 ? 3 : 4;
}

// One character of a word, as the target reads the word for itself.
struct Character {
  std::size_t at = 0;       // How many bytes of the word come before it.
  bool letter = false;      // It is one of the language's letters:
  std::uint8_t symbol = 0;  // this one.
};

// The characters of `word`. A byte that begins no character of UTF-8 is a
// character by itself, and no letter: where the character is not all
// there, is written in more bytes than it needs, is half of a pair, or
// lies past the last code point.
std::vector<Character> CharactersOf(std::string_view word, std::span<const std::uint8_t> listed) {
  std::vector<Character> characters;
  characters.reserve(word.size());
  for (std::size_t at = 0; at < word.size();) {
    const auto first = static_cast<std::uint8_t>(word[at]);
    std::size_t length = 1;
    std::uint32_t codePoint = first;
    if (first >= 0xF0) {
      length = 4;
      codePoint = first & 0x07U;
    } else if (first >= 0xE0) {
      length = 3;
      codePoint = first & 0x0FU;
    } else if (first >= 0xC0) {
      length = 2;
      codePoint = first & 0x1FU;
    }
    bool good = (first < 0x80 || first >= 0xC0) && first < 0xF8 && word.size() - at >= length;
    for (std::size_t i = 1; good && i < length; ++i) {
      const auto next = static_cast<std::uint8_t>(word[at + i]);
      good = (next & 0xC0U) == 0x80U;
      codePoint = (codePoint << 6U) | (next & 0x3FU);
    }
    good = good && BytesFor(codePoint) == length && codePoint <= 0x10FFFF && (codePoint < 0xD800 || codePoint > 0xDFFF);
    const std::uint8_t symbol = good ? LetterListed(listed, codePoint) : std::uint8_t{0};
    characters.push_back({.at = at, .letter = symbol != 0, .symbol = symbol});
    at += good ? length : 1;
  }
  return characters;
}

// A table that is none of an input's: one with an edge from its first
// state for each of twelve letters and for a word's end, and states after
// them.
const std::vector<std::uint8_t>& TableOpenedBefore() {
  static const std::vector<std::uint8_t> kTable = bn::test::PlainHyphenTable(
      {"1a", "b1", "1c2", "d3d", "e1e1", ".f1", "g1.", "hy3ph", "1in", "j2k", "k1l1m", "2l1", ".m2n."}, 1, 1);
  return kTable;
}

// A language as the target holds it: the table in memory of exactly its
// size, the same bytes again somewhere else, and a hyphenator on each. The
// second has had another table open, of which nothing may be left.
struct Language {
  explicit Language(std::span<const std::uint8_t> bytes)
      : table(bytes.begin(), bytes.end()), copy(bytes.begin(), bytes.end()) {
    BN_FUZZ_CHECK(elsewhere.Open(TableOpenedBefore()) == HyphenStatus::kOk, "the target's own table was refused");
  }

  std::vector<std::uint8_t> table;
  std::vector<std::uint8_t> copy;
  Hyphenator hyphenator;
  Hyphenator elsewhere;
  std::span<const std::uint8_t> listed;  // The table's letter list, once it has been opened.
};

// Asks where `word` may be broken, and holds the answer to everything that
// does not depend on what the patterns are. Returns the places.
std::vector<std::uint8_t> PlacesOf(Language& language, std::string_view word, std::size_t room) {
  Hyphenator& hyphenator = language.hyphenator;
  std::vector<std::uint8_t> places(Hyphenator::kMaxLetters);
  const std::size_t count = hyphenator.Find(word, places);
  const std::uint32_t work = hyphenator.LastWork();
  BN_FUZZ_CHECK(count <= places.size(), "a word was given more places than there was room for");
  places.resize(count);

  const std::vector<Character> characters = CharactersOf(word, language.listed);
  BN_FUZZ_CHECK(work <= hyphenator.MostWorkFor(characters.size()),
                "a word took more work than a word of as many characters may");
  BN_FUZZ_CHECK(characters.size() <= Hyphenator::kMaxLetters || count == 0,
                "a word too long to be looked at was given places");

  std::size_t next = 0;  // The character that the place being looked at comes before.
  for (std::size_t i = 0; i < count; ++i) {
    BN_FUZZ_CHECK(i == 0 || places[i] > places[i - 1], "a word's places are not in order");
    while (next < characters.size() && characters[next].at < places[i]) {
      ++next;
    }
    BN_FUZZ_CHECK(next < characters.size() && characters[next].at == places[i],
                  "a place is not at the start of a character of the word");
    std::size_t before = 0;
    while (before < next && characters[next - 1 - before].letter) {
      ++before;
    }
    std::size_t after = 0;
    while (next + after < characters.size() && characters[next + after].letter) {
      ++after;
    }
    BN_FUZZ_CHECK(before >= hyphenator.LeftMin(),
                  "a place has fewer letters directly before it than the language leaves");
    BN_FUZZ_CHECK(after >= hyphenator.RightMin(),
                  "a place has fewer letters directly after it than the language leaves");
  }

  // Again with less room, in memory of exactly that size.
  std::vector<std::uint8_t> fewer(room);
  const std::size_t given = hyphenator.Find(word, fewer);
  BN_FUZZ_CHECK(
      given == std::min(count, room) && std::ranges::equal(std::span<const std::uint8_t>(fewer).first(given),
                                                           std::span<const std::uint8_t>(places).first(given)),
      "with room for fewer places, the first ones were not given");
  // And by the same table where it lies somewhere else.
  std::vector<std::uint8_t> again(Hyphenator::kMaxLetters);
  const std::size_t same = language.elsewhere.Find(word, again);
  BN_FUZZ_CHECK(same == count && std::ranges::equal(std::span<const std::uint8_t>(again).first(same), places),
                "the same table somewhere else in memory gave a word other places");
  return places;
}

// A language given as a table: the bytes as they stand.
void ReadTable(bn::fuzz::Input& input, std::size_t room) {
  // How long the table says it is, by its header.
  bn::fuzz::Input ahead = input;
  const std::span<const std::uint8_t> header = ahead.Take(kHeaderBytes);
  std::size_t stated = kHeaderBytes;
  if (header.size() == kHeaderBytes) {
    stated += (Number16(header, 8) * kListedBytes) + (Number16(header, 10) * kEdgeBytes) + Number16(header, 12);
  }
  Language language(input.Take(stated));

  const HyphenStatus status = language.hyphenator.Open(language.table);
  BN_FUZZ_CHECK(
      status == language.hyphenator.Status() && (status == HyphenStatus::kOk || status == HyphenStatus::kBadTable),
      "opening a table ended with a status it cannot give");
  BN_FUZZ_CHECK(language.elsewhere.Open(language.copy) == status,
                "the same table somewhere else in memory was not taken the same way");
  const bool opened = status == HyphenStatus::kOk;
  if (opened) {
    BN_FUZZ_CHECK(language.table.size() == stated, "a table was opened that is not a header and its three pieces");
    BN_FUZZ_CHECK(language.hyphenator.LeftMin() != 0 && language.hyphenator.LeftMin() == language.table[5] &&
                      language.hyphenator.RightMin() != 0 && language.hyphenator.RightMin() == language.table[6],
                  "the fewest letters on each side of a break are not the ones the table gives");
    language.listed = LetterListOf(language.table);
  } else {
    BN_FUZZ_CHECK(language.hyphenator.MostWorkFor(Hyphenator::kMaxLetters) == 0,
                  "a table that was refused says how much work a word takes");
  }

  std::span<const std::uint8_t> rest = input.Rest();
  for (std::size_t words = 0; words < kMostWords && !rest.empty(); ++words) {
    const std::span<const std::uint8_t> line = TakeLine(rest);
    const std::vector<char> word(line.begin(), line.end());
    const std::vector<std::uint8_t> places = PlacesOf(language, {word.data(), word.size()}, room);
    BN_FUZZ_CHECK(opened || places.empty(), "a table that was refused gave a word a place");
    if (!opened) {
      continue;
    }
    // The table read by the target, an edge at a time: each stretch of
    // letters by itself, in a word that is not too long to look at.
    const std::vector<Character> characters = CharactersOf({word.data(), word.size()}, language.listed);
    std::vector<std::uint8_t> byReading;
    for (std::size_t first = 0; first < characters.size() && characters.size() <= Hyphenator::kMaxLetters;) {
      if (!characters[first].letter) {
        ++first;
        continue;
      }
      std::vector<std::uint8_t> stretch;
      std::size_t end = first;
      for (; end < characters.size() && characters[end].letter; ++end) {
        stretch.push_back(characters[end].symbol);
      }
      for (const std::size_t before : bn::test::PlacesByReadingTable(language.table, stretch)) {
        byReading.push_back(static_cast<std::uint8_t>(characters[first + before].at));
      }
      first = end;
    }
    BN_FUZZ_CHECK(places == byReading, "a word's places are not the ones reading the table an edge at a time gives");
  }
}

// The patterns in a line of an input, with whatever is not a pattern's
// made into one: a byte that cannot be in a pattern becomes a small letter,
// a pattern with letters enough is ended there, and one with no letter is
// left out.
std::vector<std::string> PatternsIn(std::span<const std::uint8_t> line) {
  std::vector<std::string> patterns;
  std::string pattern;
  std::size_t letters = 0;
  const auto end = [&] {
    if (letters != 0) {
      patterns.push_back(pattern);
    }
    pattern.clear();
    letters = 0;
  };
  for (const std::uint8_t byte : line.first(std::min(line.size(), kMostPatternBytes))) {
    if (byte == ' ') {
      end();
      continue;
    }
    const bool digit = byte >= '0' && byte <= '9';
    const bool asItIs = digit || byte == '.' || (byte >= 'a' && byte <= 'z');
    if (!digit && letters == kMostPatternLetters) {
      end();
    }
    pattern += asItIs ? static_cast<char>(byte) : static_cast<char>('a' + (byte % 26));
    letters += digit ? 0 : 1;
  }
  end();
  return patterns;
}

// A language given as patterns, which say for themselves where a word may
// be broken.
void ReadPatterns(bn::fuzz::Input& input, std::size_t room) {
  const unsigned fewest = input.Byte();
  const auto leftMin = static_cast<std::uint8_t>((fewest & 7U) + 1U);
  const auto rightMin = static_cast<std::uint8_t>(((fewest >> 3U) & 7U) + 1U);
  std::span<const std::uint8_t> rest = input.Rest();
  const std::vector<std::string> patterns = PatternsIn(TakeLine(rest));

  Language language(bn::test::PlainHyphenTable(patterns, leftMin, rightMin));
  BN_FUZZ_CHECK(language.hyphenator.Open(language.table) == HyphenStatus::kOk &&
                    language.elsewhere.Open(language.copy) == HyphenStatus::kOk,
                "a table the target made of patterns was refused");
  language.listed = LetterListOf(language.table);

  for (std::size_t words = 0; words < kMostWords && !rest.empty(); ++words) {
    // The word as it is asked about, and in small letters.
    std::string written;
    std::string small;
    for (const std::uint8_t byte : TakeLine(rest)) {
      const bool capital = byte >= 'A' && byte <= 'Z';
      const bool asItIs = capital || (byte >= 'a' && byte <= 'z');
      const char letter = asItIs ? static_cast<char>(byte) : static_cast<char>('a' + (byte % 26));
      written += letter;
      small += capital ? static_cast<char>(letter - 'A' + 'a') : letter;
    }
    const std::vector<char> word(written.begin(), written.end());
    const std::vector<std::uint8_t> places = PlacesOf(language, {word.data(), word.size()}, room);
    std::vector<std::size_t> byHand;
    if (small.size() <= Hyphenator::kMaxLetters) {
      byHand = bn::test::PlacesByHand(patterns, small, leftMin, rightMin);
    }
    BN_FUZZ_CHECK(std::ranges::equal(places, byHand), "a word's places are not the ones its patterns give it");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::size_t room = kRoom[(choices >> 1U) & 7U];
  if ((choices & 1U) != 0) {
    ReadPatterns(input, room);
  } else {
    ReadTable(input, room);
  }
  return 0;
}
