// bn-hyphen: makes the table of a language's hyphenation patterns that the
// firmware reads, and breaks words with one, by the code the firmware
// breaks them with.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hyphen/pack.h"
#include "layout/hyphenator.h"

namespace {

using bn::hyphen_tool::Pack;
using bn::hyphen_tool::Packed;
using bn::hyphen_tool::PackOptions;
using bn::layout::Hyphenator;
using bn::layout::HyphenStatus;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s pack PATTERNS --out TABLE [--exceptions FILE] [--letters FILE]\n"
               "                 [--left N] [--right N]\n"
               "       %s words TABLE [FILE] [--count]\n"
               "       %s info TABLE\n"
               "\n"
               "Hyphenation patterns, as the firmware uses them.\n"
               "\n"
               "  pack  makes a table from a language's patterns as they are written for TeX,\n"
               "        one pattern after another with spaces or line ends between: hy3ph,\n"
               "        .un1, 4m1p. Anything from a %% to the end of its line is passed over.\n"
               "\n"
               "  --out TABLE         where the table is written\n"
               "  --exceptions FILE   words written out with a - wherever they may be broken,\n"
               "                      which no pattern is then let change: ta-ble\n"
               "  --letters FILE      a line for each letter that has other forms: the letter\n"
               "                      as the patterns write it, then the characters that are\n"
               "                      to count as it: aA. Without it, a to z have their\n"
               "                      capitals and no other letter has any\n"
               "  --left N            the fewest letters before a break (default: 2)\n"
               "  --right N           the fewest letters after one (default: 3)\n"
               "\n"
               "  words breaks words by a table: one word a line, from FILE or, without one,\n"
               "        from what it is given to read. Each is printed with a - at every\n"
               "        place it may be broken.\n"
               "\n"
               "  --count             print no words, only how many there were and what they\n"
               "                      took\n"
               "\n"
               "  info  says what a table holds.\n"
               "\n"
               "Ends with 0 if all went well, 1 if the patterns cannot be made into a table,\n"
               "the table is not one, or a file cannot be read or written.\n",
               program, program, program);
}

bool ReadFile(const std::string& path, std::string& contents) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "bn-hyphen: cannot read %s\n", path.c_str());
    return false;
  }
  contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// The number an option is given, from 1 to 255. False if it is not one.
bool SmallNumber(const std::string& text, std::uint8_t& number) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0' || value < 1 || value > 255) {
    return false;
  }
  number = static_cast<std::uint8_t>(value);
  return true;
}

int RunPack(const std::vector<std::string>& arguments) {
  std::string patternsPath;
  std::string exceptionsPath;
  std::string lettersPath;
  std::string outPath;
  PackOptions options;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    const bool hasValue = i + 1 < arguments.size();
    if (argument == "--out" && hasValue) {
      outPath = arguments[++i];
    } else if (argument == "--exceptions" && hasValue) {
      exceptionsPath = arguments[++i];
    } else if (argument == "--letters" && hasValue) {
      lettersPath = arguments[++i];
    } else if ((argument == "--left" || argument == "--right") && hasValue) {
      if (!SmallNumber(arguments[++i], argument == "--left" ? options.leftMin : options.rightMin)) {
        return -1;
      }
    } else if (!argument.starts_with("--") && patternsPath.empty()) {
      patternsPath = argument;
    } else {
      return -1;
    }
  }
  if (patternsPath.empty() || outPath.empty()) {
    return -1;
  }

  std::string patterns;
  std::string exceptions;
  std::string letters;
  if (!ReadFile(patternsPath, patterns) || (!exceptionsPath.empty() && !ReadFile(exceptionsPath, exceptions)) ||
      (!lettersPath.empty() && !ReadFile(lettersPath, letters))) {
    return 1;
  }
  const Packed packed = Pack(patterns, exceptions, letters, options);
  if (!packed.ok) {
    std::fprintf(stderr, "bn-hyphen: %s\n", packed.error.c_str());
    return 1;
  }
  std::ofstream out(outPath, std::ios::binary);
  out.write(reinterpret_cast<const char*>(packed.table.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(packed.table.size()));
  out.close();
  if (!out) {
    std::fprintf(stderr, "bn-hyphen: cannot write %s\n", outPath.c_str());
    return 1;
  }
  std::printf("patterns %zu, exceptions %zu\n", packed.patterns, packed.exceptions);
  std::printf("letters %zu, as %zu characters\n", packed.letters, packed.listed);
  std::printf("states %zu, edges %zu (%zu before like branches were kept once), widest %zu\n", packed.states,
              packed.edges, packed.edgesUnshared, packed.widest);
  std::printf("longest pattern %zu, values %zu bytes\n", packed.deepest, packed.valueBytes);
  std::printf("table %zu bytes\n", packed.table.size());
  return 0;
}

bool OpenTable(const std::string& path, std::string& bytes, Hyphenator& hyphenator) {
  if (!ReadFile(path, bytes)) {
    return false;
  }
  const std::span<const std::uint8_t> table(
      reinterpret_cast<const std::uint8_t*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      bytes.size());
  const HyphenStatus status = hyphenator.Open(table);
  if (status != HyphenStatus::kOk) {
    std::fprintf(stderr, "bn-hyphen: %s: %s\n", path.c_str(), bn::layout::HyphenStatusName(status));
    return false;
  }
  return true;
}

int RunWords(const std::vector<std::string>& arguments) {
  std::string tablePath;
  std::string wordsPath;
  bool countOnly = false;
  for (const std::string& argument : arguments) {
    if (argument == "--count") {
      countOnly = true;
    } else if (!argument.starts_with("--") && tablePath.empty()) {
      tablePath = argument;
    } else if (!argument.starts_with("--") && wordsPath.empty()) {
      wordsPath = argument;
    } else {
      return -1;
    }
  }
  if (tablePath.empty()) {
    return -1;
  }
  std::string bytes;
  Hyphenator hyphenator;
  if (!OpenTable(tablePath, bytes, hyphenator)) {
    return 1;
  }
  std::ifstream file;
  if (!wordsPath.empty()) {
    file.open(wordsPath, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "bn-hyphen: cannot read %s\n", wordsPath.c_str());
      return 1;
    }
  }
  std::istream& in = wordsPath.empty() ? std::cin : file;

  std::size_t words = 0;
  std::size_t broken = 0;
  std::size_t placesInAll = 0;
  std::uint64_t workInAll = 0;
  std::uint32_t mostWork = 0;
  std::array<std::uint8_t, Hyphenator::kMaxLetters> places{};
  std::string word;
  while (std::getline(in, word)) {
    if (!word.empty() && word.back() == '\r') {
      word.pop_back();
    }
    const std::size_t count = hyphenator.Find(word, places);
    ++words;
    broken += count != 0 ? 1U : 0U;
    placesInAll += count;
    workInAll += hyphenator.LastWork();
    mostWork = std::max(mostWork, hyphenator.LastWork());
    if (countOnly) {
      continue;
    }
    std::string shown;
    std::size_t next = 0;
    for (std::size_t at = 0; at < word.size(); ++at) {
      if (next < count && places[next] == at) {
        shown += '-';
        ++next;
      }
      shown += word[at];
    }
    std::printf("%s\n", shown.c_str());
  }
  if (countOnly) {
    std::printf("words %zu, with a place to break %zu, places %zu\n", words, broken, placesInAll);
    std::printf("work %llu, most for one word %u, and no more than %u for any\n",
                static_cast<unsigned long long>(workInAll), mostWork, hyphenator.MostWorkFor(Hyphenator::kMaxLetters));
  }
  return 0;
}

int RunInfo(const std::vector<std::string>& arguments) {
  if (arguments.size() != 1) {
    return -1;
  }
  std::string bytes;
  Hyphenator hyphenator;
  if (!OpenTable(arguments[0], bytes, hyphenator)) {
    return 1;
  }
  const auto byte = [&](std::size_t at) { return static_cast<unsigned>(static_cast<std::uint8_t>(bytes[at])); };
  const auto pair = [&](std::size_t at) { return byte(at) | (byte(at + 1) << 8U); };
  std::printf("table %zu bytes, version %u\n", bytes.size(), byte(4));
  std::printf("letters before a break %u, after one %u\n", byte(5), byte(6));
  std::printf("characters that are letters %u\n", pair(8));
  std::printf("edges %u, widest state %u, longest pattern %u\n", pair(10), byte(14), byte(7));
  std::printf("values %u bytes\n", pair(12));
  std::printf("most work for a word of %zu letters %u\n", Hyphenator::kMaxLetters,
              hyphenator.MostWorkFor(Hyphenator::kMaxLetters));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> all(argv, argv + argc);
  int result = -1;
  if (all.size() >= 2) {
    const std::vector<std::string> arguments(all.begin() + 2, all.end());
    if (all[1] == "pack") {
      result = RunPack(arguments);
    } else if (all[1] == "words") {
      result = RunWords(arguments);
    } else if (all[1] == "info") {
      result = RunInfo(arguments);
    }
  }
  if (result < 0) {
    PrintUsage(all.empty() ? "bn-hyphen" : all[0].c_str());
    return 1;
  }
  return result;
}
