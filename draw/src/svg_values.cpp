#include "draw/svg_values.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace bn::draw {

namespace {

constexpr bool IsLetter(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr char Lowered(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The part of `text` from `at` on.
std::string_view From(std::string_view text, std::size_t at) noexcept {
  text.remove_prefix(at);
  return text;
}

// The first `count` bytes of `text`.
std::string_view First(std::string_view text, std::size_t count) noexcept {
  text.remove_suffix(text.size() - count);
  return text;
}

void SkipSpace(std::string_view text, std::size_t& at) noexcept {
  while (at < text.size() && IsSvgSpace(text[at])) {
    ++at;
  }
}

// Space, and one comma if there is one.
void SkipSeparator(std::string_view text, std::size_t& at) noexcept {
  SkipSpace(text, at);
  if (at < text.size() && text[at] == ',') {
    ++at;
    SkipSpace(text, at);
  }
}

// A number as a whole number of 4096ths, however large.
std::int64_t InFixed(Number number) noexcept {
  constexpr int kShift = 32 - kFixedBits;
  constexpr Number kHalf = 524288;
  return ShiftDown(number + kHalf, kShift);
}

// ---- Colours ----

struct NamedColour {
  std::string_view name;
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

// CSS's seventeen basic colours, with the other spelling of gray and the
// other names of aqua and fuchsia. The books draw in black and in colours
// written as numbers: of all the names, the only one they use is black.
constexpr std::array<NamedColour, 20> kNamedColours = {{
    {.name = "aqua", .red = 0, .green = 255, .blue = 255},     {.name = "black", .red = 0, .green = 0, .blue = 0},
    {.name = "blue", .red = 0, .green = 0, .blue = 255},       {.name = "cyan", .red = 0, .green = 255, .blue = 255},
    {.name = "fuchsia", .red = 255, .green = 0, .blue = 255},  {.name = "gray", .red = 128, .green = 128, .blue = 128},
    {.name = "green", .red = 0, .green = 128, .blue = 0},      {.name = "grey", .red = 128, .green = 128, .blue = 128},
    {.name = "lime", .red = 0, .green = 255, .blue = 0},       {.name = "magenta", .red = 255, .green = 0, .blue = 255},
    {.name = "maroon", .red = 128, .green = 0, .blue = 0},     {.name = "navy", .red = 0, .green = 0, .blue = 128},
    {.name = "olive", .red = 128, .green = 128, .blue = 0},    {.name = "orange", .red = 255, .green = 165, .blue = 0},
    {.name = "purple", .red = 128, .green = 0, .blue = 128},   {.name = "red", .red = 255, .green = 0, .blue = 0},
    {.name = "silver", .red = 192, .green = 192, .blue = 192}, {.name = "teal", .red = 0, .green = 128, .blue = 128},
    {.name = "white", .red = 255, .green = 255, .blue = 255},  {.name = "yellow", .red = 255, .green = 255, .blue = 0},
}};

bool HexDigit(char c, std::uint32_t& digit) noexcept {
  if (c >= '0' && c <= '9') {
    digit = static_cast<std::uint32_t>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    digit = static_cast<std::uint32_t>(c - 'a') + 10;
  } else if (c >= 'A' && c <= 'F') {
    digit = static_cast<std::uint32_t>(c - 'A') + 10;
  } else {
    return false;
  }
  return true;
}

// "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa", without its "#".
bool ParseHex(std::string_view digits, std::uint8_t& gray, std::uint8_t& alpha) noexcept {
  const bool brief = digits.size() == 3 || digits.size() == 4;
  if (!brief && digits.size() != 6 && digits.size() != 8) {
    return false;
  }
  std::array<std::uint32_t, 4> parts = {0, 0, 0, 255};
  const std::size_t count = brief ? digits.size() : digits.size() / 2;
  for (std::size_t part = 0; part < count; ++part) {
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    if (brief) {
      // One digit stands for two the same: "f" is "ff".
      if (!HexDigit(digits[part], high)) {
        return false;
      }
      low = high;
    } else if (!HexDigit(digits[2 * part], high) || !HexDigit(digits[(2 * part) + 1], low)) {
      return false;
    }
    parts[part] = (high * 16) + low;
  }
  gray = GrayOf(static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                static_cast<std::uint8_t>(parts[2]));
  alpha = static_cast<std::uint8_t>(parts[3]);
  return true;
}

// A part of a colour as written in "rgb(...)": a number from 0 to 255 or a
// percentage, as 0 to 255. Or, with `ofOne`, a number from 0 to 1.
bool ScanPart(std::string_view text, std::size_t& at, bool ofOne, std::uint8_t& part) noexcept {
  Number number = 0;
  bool clamped = false;
  if (!ScanNumber(text, at, number, clamped)) {
    return false;
  }
  // In 4096ths of the whole that 255 stands for.
  std::int64_t value = InFixed(number);
  if (at < text.size() && text[at] == '%') {
    ++at;
    value = (value * 255) / 100;
  } else if (ofOne) {
    value *= 255;
  }
  constexpr std::int64_t kHalf = kFixedOne / 2;
  part = static_cast<std::uint8_t>(std::clamp<std::int64_t>(ShiftDown(value + kHalf, kFixedBits), 0, 255));
  return true;
}

// What is in the brackets of "rgb(...)" or "rgba(...)", and the bracket
// that closes them: three parts, and perhaps how solid it is, with spaces,
// commas or a stroke between them.
bool ParseParts(std::string_view text, std::uint8_t& gray, std::uint8_t& alpha) noexcept {
  std::array<std::uint8_t, 3> parts{};
  std::size_t at = 0;
  for (std::uint8_t& part : parts) {
    SkipSeparator(text, at);
    if (!ScanPart(text, at, false, part)) {
      return false;
    }
  }
  SkipSeparator(text, at);
  std::uint8_t solid = 255;
  if (at < text.size() && text[at] != ')') {
    if (text[at] == '/') {
      ++at;
      SkipSpace(text, at);
    }
    if (!ScanPart(text, at, true, solid)) {
      return false;
    }
    SkipSpace(text, at);
  }
  if (at + 1 != text.size() || text[at] != ')') {
    return false;
  }
  gray = GrayOf(parts[0], parts[1], parts[2]);
  alpha = solid;
  return true;
}

}  // namespace

std::string_view WithoutSpace(std::string_view text) noexcept {
  std::size_t from = 0;
  std::size_t to = text.size();
  while (from < to && IsSvgSpace(text[from])) {
    ++from;
  }
  while (to > from && IsSvgSpace(text[to - 1])) {
    --to;
  }
  return {text.data() + from, to - from};
}

bool IsWord(std::string_view text, std::string_view word) noexcept {
  if (text.size() != word.size()) {
    return false;
  }
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (Lowered(text[at]) != word[at]) {
      return false;
    }
  }
  return true;
}

bool ParseSvgLength(std::string_view text, SvgLength& length, bool& clamped) noexcept {
  text = WithoutSpace(text);
  // The unit is the letters at the end, or a percent sign. It is taken off
  // first, so that the "e" of "em" is not read as the start of an exponent.
  std::size_t split = text.size();
  while (split > 0 && (IsLetter(text[split - 1]) || text[split - 1] == '%')) {
    --split;
  }
  const std::string_view unit = From(text, split);
  SvgUnit kind = SvgUnit::kNone;
  if (unit.empty()) {
    kind = SvgUnit::kNone;
  } else if (unit == "%") {
    kind = SvgUnit::kPercent;
  } else if (IsWord(unit, "px")) {
    kind = SvgUnit::kPx;
  } else if (IsWord(unit, "pt")) {
    kind = SvgUnit::kPt;
  } else if (IsWord(unit, "pc")) {
    kind = SvgUnit::kPc;
  } else if (IsWord(unit, "mm")) {
    kind = SvgUnit::kMm;
  } else if (IsWord(unit, "cm")) {
    kind = SvgUnit::kCm;
  } else if (IsWord(unit, "in")) {
    kind = SvgUnit::kIn;
  } else if (IsWord(unit, "em")) {
    kind = SvgUnit::kEm;
  } else if (IsWord(unit, "ex")) {
    kind = SvgUnit::kEx;
  } else {
    return false;
  }
  const std::string_view digits = First(text, split);
  std::size_t at = 0;
  Number value = 0;
  if (!ScanNumber(digits, at, value, clamped) || at != digits.size()) {
    return false;
  }
  length = {.value = value, .unit = kind};
  return true;
}

bool ParseSvgLengths(std::string_view text, std::span<SvgLength> lengths, std::size_t& count, bool& clamped) noexcept {
  count = 0;
  std::size_t at = 0;
  bool comma = true;  // Where a comma may come: not first, nor two in a row.
  while (true) {
    while (at < text.size() && IsSvgSpace(text[at])) {
      ++at;
    }
    if (at == text.size()) {
      // Nor last.
      return count > 0 && !comma;
    }
    if (text[at] == ',') {
      if (comma) {
        return false;
      }
      comma = true;
      ++at;
      continue;
    }
    std::size_t end = at;
    while (end < text.size() && !IsSvgSpace(text[end]) && text[end] != ',') {
      ++end;
    }
    if (count == lengths.size() || !ParseSvgLength({text.data() + at, end - at}, lengths[count], clamped)) {
      return false;
    }
    ++count;
    comma = false;
    at = end;
  }
}

Fixed LengthIn(const SvgLength& length, Fixed whole, bool& clamped) noexcept {
  const std::int64_t units = InFixed(length.value);
  switch (length.unit) {
    case SvgUnit::kNone:
    case SvgUnit::kPx:
      return Clamp32(units, clamped);
    case SvgUnit::kPercent:
      return Clamp32((units * whole) / (100 * std::int64_t{kFixedOne}), clamped);
    case SvgUnit::kPt:
      return Clamp32((units * 4) / 3, clamped);
    case SvgUnit::kPc:
    case SvgUnit::kEm:
      return Clamp32(units * 16, clamped);
    case SvgUnit::kMm:
      // An inch is 25.4 mm and 96 pixels.
      return Clamp32((units * 480) / 127, clamped);
    case SvgUnit::kCm:
      return Clamp32((units * 4800) / 127, clamped);
    case SvgUnit::kIn:
      return Clamp32(units * 96, clamped);
    case SvgUnit::kEx:
      return Clamp32(units * 8, clamped);
  }
  return 0;
}

bool ParseSvgColour(std::string_view text, std::uint8_t& gray, std::uint8_t& alpha) noexcept {
  text = WithoutSpace(text);
  if (text.empty()) {
    return false;
  }
  if (text[0] == '#') {
    return ParseHex(From(text, 1), gray, alpha);
  }
  if (text.size() > 4 && IsWord(First(text, 4), "rgb(")) {
    return ParseParts(From(text, 4), gray, alpha);
  }
  if (text.size() > 5 && IsWord(First(text, 5), "rgba(")) {
    return ParseParts(From(text, 5), gray, alpha);
  }
  if (IsWord(text, "transparent")) {
    gray = 0;
    alpha = 0;
    return true;
  }
  for (const NamedColour& colour : kNamedColours) {
    if (IsWord(text, colour.name)) {
      gray = GrayOf(colour.red, colour.green, colour.blue);
      alpha = 255;
      return true;
    }
  }
  return false;
}

bool ParseSvgPaint(std::string_view text, SvgPaint& paint) noexcept {
  text = WithoutSpace(text);
  SvgPaint read;
  if (text.size() > 4 && IsWord(First(text, 4), "url(")) {
    // A reference, and after its bracket what to use if there is nothing
    // at the other end of it.
    std::size_t at = 4;
    while (at < text.size() && text[at] != ')') {
      ++at;
    }
    if (at == text.size()) {
      return false;
    }
    read.server = true;
    read.kind = SvgPaint::Kind::kNone;
    text = WithoutSpace(From(text, at + 1));
    if (text.empty()) {
      paint = read;
      return true;
    }
  }
  if (IsWord(text, "none")) {
    read.kind = SvgPaint::Kind::kNone;
  } else if (IsWord(text, "currentcolor")) {
    read.kind = SvgPaint::Kind::kCurrent;
  } else if (IsWord(text, "inherit") && !read.server) {
    read.kind = SvgPaint::Kind::kInherit;
  } else if (ParseSvgColour(text, read.gray, read.alpha)) {
    read.kind = SvgPaint::Kind::kGray;
  } else {
    return false;
  }
  paint = read;
  return true;
}

bool ParseSvgOpacity(std::string_view text, std::uint8_t& opacity) noexcept {
  text = WithoutSpace(text);
  std::size_t at = 0;
  Number number = 0;
  bool clamped = false;
  if (!ScanNumber(text, at, number, clamped)) {
    return false;
  }
  if (at + 1 == text.size() && text[at] == '%') {
    number /= 100;
  } else if (at != text.size()) {
    return false;
  }
  const Number held = std::clamp<Number>(number, 0, kNumberOne);
  constexpr Number kHalf = kNumberOne / 2;
  opacity = static_cast<std::uint8_t>(ShiftDown((held * 255) + kHalf, 32));
  return true;
}

bool ParseSvgViewBox(std::string_view text, SvgBox& box, bool& clamped) noexcept {
  text = WithoutSpace(text);
  std::array<Fixed, 4> numbers{};
  std::size_t at = 0;
  for (std::size_t index = 0; index < numbers.size(); ++index) {
    if (index > 0) {
      SkipSeparator(text, at);
    }
    Number number = 0;
    if (!ScanNumber(text, at, number, clamped)) {
      return false;
    }
    numbers[index] = FixedOf(number, clamped);
  }
  if (at != text.size()) {
    return false;
  }
  box = {.x = numbers[0], .y = numbers[1], .width = numbers[2], .height = numbers[3]};
  return true;
}

bool ParseSvgFit(std::string_view text, SvgFit& fit) noexcept {
  text = WithoutSpace(text);
  // Up to three words: "defer", which is for pictures and changes nothing
  // here, then where the box goes, then "meet" or "slice".
  std::array<std::string_view, 3> words{};
  std::size_t count = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t from = at;
    while (at < text.size() && !IsSvgSpace(text[at])) {
      ++at;
    }
    if (count == words.size()) {
      return false;
    }
    words[count] = {text.data() + from, at - from};
    ++count;
    SkipSpace(text, at);
  }
  std::size_t next = 0;
  if (count > 0 && words[0] == "defer") {
    next = 1;
  }
  if (next >= count) {
    return false;
  }

  SvgFit read;
  const std::string_view where = words[next];
  ++next;
  if (where == "none") {
    read.stretch = true;
  } else {
    // "xMid" and "YMax", say: four letters each.
    if (where.size() != 8 || where[0] != 'x' || where[4] != 'Y') {
      return false;
    }
    const auto align = [](std::string_view part, SvgFit::Align& to) noexcept {
      if (part == "Min") {
        to = SvgFit::Align::kMin;
      } else if (part == "Mid") {
        to = SvgFit::Align::kMid;
      } else if (part == "Max") {
        to = SvgFit::Align::kMax;
      } else {
        return false;
      }
      return true;
    };
    if (!align({where.data() + 1, 3}, read.x) || !align({where.data() + 5, 3}, read.y)) {
      return false;
    }
  }
  if (next < count) {
    if (words[next] == "slice") {
      read.slice = true;
    } else if (words[next] != "meet") {
      return false;
    }
    ++next;
  }
  if (next != count) {
    return false;
  }
  fit = read;
  return true;
}

Matrix FitBox(const SvgBox& box, const SvgBox& space, const SvgFit& fit, bool& clamped) noexcept {
  // To the nearest that a factor can be, so that a box thousands of units
  // wide still ends within a few hundredths of a pixel of where it should.
  const auto ratio = [&clamped](Fixed over, Fixed under) noexcept {
    const std::int64_t by = std::max<Fixed>(under, 1);
    return Clamp32(((std::int64_t{over} * kCoefficientOne) + (by / 2)) / by, clamped);
  };
  Coefficient scaleX = ratio(space.width, box.width);
  Coefficient scaleY = ratio(space.height, box.height);
  if (!fit.stretch) {
    scaleX = fit.slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    scaleY = scaleX;
  }
  // How long a length of the box comes out, and where the box goes in
  // what is left over of the space.
  const auto scaled = [](Fixed length, Coefficient by) noexcept {
    constexpr std::int64_t kHalf = kCoefficientOne / 2;
    return ShiftDown((std::int64_t{length} * by) + kHalf, kCoefficientBits);
  };
  const auto placed = [](std::int64_t spare, SvgFit::Align align) noexcept -> std::int64_t {
    switch (align) {
      case SvgFit::Align::kMin:
        return 0;
      case SvgFit::Align::kMid:
        return spare / 2;
      case SvgFit::Align::kMax:
        return spare;
    }
    return 0;
  };
  Matrix matrix;
  matrix.a = scaleX;
  matrix.d = scaleY;
  matrix.e = Clamp32(space.x - scaled(box.x, scaleX) +
                         placed(space.width - scaled(box.width, scaleX), fit.stretch ? SvgFit::Align::kMin : fit.x),
                     clamped);
  matrix.f = Clamp32(space.y - scaled(box.y, scaleY) +
                         placed(space.height - scaled(box.height, scaleY), fit.stretch ? SvgFit::Align::kMin : fit.y),
                     clamped);
  return matrix;
}

}  // namespace bn::draw
