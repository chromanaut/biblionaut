#include "font/preset.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>

namespace bn::font_tool {

namespace {

constexpr std::uint16_t kSmallestSize = 6;
constexpr std::uint16_t kLargestSize = 96;

constexpr std::array<std::string_view, 4> kRoleNames = {"serif", "sans", "mono", "fallback"};
constexpr std::array<std::string_view, 4> kStyleNames = {"regular", "bold", "italic", "bold-italic"};

std::string_view Trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// The first word of `text`, and what is left after it and the spaces that
// follow.
std::string_view FirstWord(std::string_view& text) {
  const std::size_t end = std::min(text.find_first_of(" \t"), text.size());
  const std::string_view word = text.substr(0, end);
  text = Trimmed(text.substr(end));
  return word;
}

bool ReadCodePoint(std::string_view text, std::uint32_t& value) {
  if (text.size() < 3 || (text[0] != 'U' && text[0] != 'u') || text[1] != '+') {
    return false;
  }
  const std::string_view digits = text.substr(2);
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
  return error == std::errc{} && end == digits.data() + digits.size() && value <= 0x10FFFF;
}

std::string At(std::size_t line, const std::string& what) { return "line " + std::to_string(line) + ": " + what; }

}  // namespace

bool ReadCodePoints(std::string_view text, CodePoints& codePoints) {
  const std::size_t dash = text.find('-');
  std::uint32_t first = 0;
  if (dash == std::string_view::npos) {
    if (!ReadCodePoint(text, first)) {
      return false;
    }
    codePoints = {first, first};
    return true;
  }
  std::uint32_t last = 0;
  if (!ReadCodePoint(text.substr(0, dash), first) || !ReadCodePoint(text.substr(dash + 1), last) || last < first) {
    return false;
  }
  codePoints = {first, last};
  return true;
}

std::string_view RoleName(draw::FaceRole role) { return kRoleNames.at(static_cast<std::size_t>(role)); }
std::string_view StyleName(std::uint8_t style) { return kStyleNames.at(style); }

const std::vector<CodePoints>& DefaultCharacters() {
  static const std::vector<CodePoints> kDefault = {
      {0x0020, 0x007E},  // Basic Latin
      {0x00A0, 0x024F},  // Latin-1, Latin Extended-A and -B
      {0x0250, 0x02FF},  // IPA, spacing modifier letters
      {0x0300, 0x036F},  // Combining marks
      {0x0370, 0x03FF},  // Greek and Coptic
      {0x0400, 0x052F},  // Cyrillic and its supplement
      {0x1E00, 0x1EFF},  // Latin Extended Additional
      {0x1F00, 0x1FFF},  // Greek Extended
      {0x2000, 0x206F},  // General punctuation
      {0x20A0, 0x20CF},  // Currency
      {0x2100, 0x218F},  // Letterlike symbols, number forms
      {0x2190, 0x21FF},  // Arrows
      {0x2200, 0x22FF},  // Mathematical operators
      {0xFB00, 0xFB06},  // Latin ligatures
      {0xFFFD, 0xFFFD},  // The replacement character, drawn for a byte that is not UTF-8
  };
  return kDefault;
}

namespace {

// Adds what one preset's text says to `preset`. False, with the error said,
// at the first line that is wrong.
bool AddPreset(Preset& preset, std::string_view text, std::string_view folder) {
  std::size_t lineNumber = 0;
  while (!text.empty()) {
    ++lineNumber;
    const std::size_t end = std::min(text.find('\n'), text.size());
    std::string_view line = text.substr(0, end);
    text = text.substr(std::min(end + 1, text.size()));
    line = Trimmed(line.substr(0, std::min(line.find('#'), line.size())));
    if (line.empty()) {
      continue;
    }
    const std::string_view word = FirstWord(line);
    if (word == "sizes") {
      while (!line.empty()) {
        const std::string_view number = FirstWord(line);
        unsigned size = 0;
        const auto [at, error] = std::from_chars(number.data(), number.data() + number.size(), size);
        if (error != std::errc{} || at != number.data() + number.size() || size < kSmallestSize ||
            size > kLargestSize) {
          preset.error = At(lineNumber, "a size is a number of pixels from " + std::to_string(kSmallestSize) + " to " +
                                            std::to_string(kLargestSize) + ", not '" + std::string(number) + "'");
          return false;
        }
        preset.sizes.push_back(static_cast<std::uint16_t>(size));
      }
      continue;
    }
    if (word == "characters") {
      while (!line.empty()) {
        const std::string_view range = FirstWord(line);
        CodePoints codePoints;
        if (!ReadCodePoints(range, codePoints)) {
          preset.error =
              At(lineNumber, "characters are written U+0041 or U+0041-U+005A, not '" + std::string(range) + "'");
          return false;
        }
        preset.characters.push_back(codePoints);
      }
      continue;
    }
    const auto role = std::find(kRoleNames.begin(), kRoleNames.end(), word);
    if (role == kRoleNames.end()) {
      preset.error = At(lineNumber,
                        "a line begins with sizes, characters, or a role (serif, sans, mono, "
                        "fallback), not '" +
                            std::string(word) + "'");
      return false;
    }
    const std::string_view styleWord = FirstWord(line);
    const auto style = std::find(kStyleNames.begin(), kStyleNames.end(), styleWord);
    if (style == kStyleNames.end()) {
      preset.error =
          At(lineNumber, "a style is regular, italic, bold or bold-italic, not '" + std::string(styleWord) + "'");
      return false;
    }
    if (line.empty()) {
      preset.error = At(lineNumber, "no font file is named");
      return false;
    }
    FaceFile face{.role = static_cast<draw::FaceRole>(role - kRoleNames.begin()),
                  .style = static_cast<std::uint8_t>(style - kStyleNames.begin()),
                  .path = line.front() == '/' || folder.empty() ? std::string(line)
                                                                : std::string(folder) + "/" + std::string(line),
                  .line = lineNumber};
    for (const FaceFile& named : preset.faces) {
      if (named.role == face.role && named.style == face.style) {
        preset.error = At(lineNumber, std::string(RoleName(face.role)) + " " + std::string(StyleName(face.style)) +
                                          " was named already, on line " + std::to_string(named.line));
        return false;
      }
    }
    preset.faces.push_back(std::move(face));
  }
  return true;
}

// What has to be so of a preset once all of it is read.
void FinishPreset(Preset& preset) {
  if (preset.sizes.empty()) {
    preset.error = "no sizes are given";
    return;
  }
  if (preset.faces.empty()) {
    preset.error = "no faces are named";
    return;
  }
  std::sort(preset.sizes.begin(), preset.sizes.end());
  preset.sizes.erase(std::unique(preset.sizes.begin(), preset.sizes.end()), preset.sizes.end());
  preset.ok = true;
}

}  // namespace

Preset ReadPreset(std::string_view text, std::string_view folder) {
  Preset preset;
  if (AddPreset(preset, text, folder)) {
    FinishPreset(preset);
  }
  return preset;
}

Preset ReadPresets(std::span<const PresetText> presets) {
  Preset preset;
  for (const PresetText& one : presets) {
    if (!AddPreset(preset, one.text, one.folder)) {
      preset.error.insert(0, one.name + ": ");
      return preset;
    }
  }
  FinishPreset(preset);
  return preset;
}

}  // namespace bn::font_tool
