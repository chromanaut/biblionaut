#include "epub/css.h"

#include <algorithm>
#include <cstring>

#include "core/metrics.h"
#include "names.h"

namespace bn::epub {

namespace {

// Sheets read, and rules told of in them.
core::Counter cssSheets{"css_sheets"};
core::Counter cssRules{"css_rules"};

constexpr bool IsSpace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }

constexpr bool IsDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

constexpr bool IsHex(char ch) noexcept { return IsDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

// A byte that can be in a name: a letter, a digit, `-`, `_`, or part of a
// character beyond ASCII.
constexpr bool IsNameByte(char ch) noexcept {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || IsDigit(ch) || ch == '-' || ch == '_' ||
         static_cast<unsigned char>(ch) >= 0x80U;
}

std::string_view Trimmed(std::string_view text) noexcept {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::span<char> Trimmed(std::span<char> text) noexcept {
  while (!text.empty() && IsSpace(text.front())) {
    text = text.subspan(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text = text.first(text.size() - 1);
  }
  return text;
}

std::string_view ViewOf(std::span<const char> text) noexcept { return {text.data(), text.size()}; }

// The first `count` bytes of `text`, or all of it; and what comes after
// them, or nothing.
std::string_view First(std::string_view text, std::size_t count) noexcept {
  text.remove_suffix(text.size() - std::min(count, text.size()));
  return text;
}

std::string_view After(std::string_view text, std::size_t count) noexcept {
  text.remove_prefix(std::min(count, text.size()));
  return text;
}

// Who is told, and what is counted. A `style` attribute is read with no
// counts to keep.
struct Sink {
  CssHandler& handler;
  std::uint32_t* declarations = nullptr;
  std::array<std::uint32_t, kCssDroppedKinds>* dropped = nullptr;

  void Drop(CssDropped what, std::string_view text) const noexcept {
    if (dropped != nullptr) {
      ++(*dropped)[static_cast<std::size_t>(what)];
    }
    handler.OnDropped(what, text);
  }

  void Declare(CssProperty property, const CssValue& value, bool important) const noexcept {
    if (declarations != nullptr) {
      ++*declarations;
    }
    handler.OnDeclaration({.property = property, .value = value, .important = important});
  }
};

// ---- Values ----

struct Word {
  std::string_view name;
  CssKeyword keyword;
};

// The words every property takes.
constexpr std::array<Word, 3> kEverywhere = {
    {{"inherit", CssKeyword::kInherit}, {"unset", CssKeyword::kInherit}, {"initial", CssKeyword::kInitial}}};

constexpr std::array<Word, 3> kFontStyles = {
    {{"normal", CssKeyword::kNormal}, {"italic", CssKeyword::kItalic}, {"oblique", CssKeyword::kOblique}}};
constexpr std::array<Word, 4> kFontWeights = {{{"normal", CssKeyword::kNormal},
                                               {"bold", CssKeyword::kBold},
                                               {"bolder", CssKeyword::kBolder},
                                               {"lighter", CssKeyword::kLighter}}};
constexpr std::array<Word, 2> kFontVariants = {
    {{"normal", CssKeyword::kNormal}, {"small-caps", CssKeyword::kSmallCaps}}};
constexpr std::array<Word, 9> kFontSizes = {{{"xx-small", CssKeyword::kXxSmall},
                                             {"x-small", CssKeyword::kXSmall},
                                             {"small", CssKeyword::kSmall},
                                             {"medium", CssKeyword::kMedium},
                                             {"large", CssKeyword::kLarge},
                                             {"x-large", CssKeyword::kXLarge},
                                             {"xx-large", CssKeyword::kXxLarge},
                                             {"smaller", CssKeyword::kSmaller},
                                             {"larger", CssKeyword::kLarger}}};
constexpr std::array<Word, 1> kNormalOnly = {{{"normal", CssKeyword::kNormal}}};
constexpr std::array<Word, 1> kAutoOnly = {{{"auto", CssKeyword::kAuto}}};
constexpr std::array<Word, 6> kTextAligns = {{{"left", CssKeyword::kLeft},
                                              {"right", CssKeyword::kRight},
                                              {"center", CssKeyword::kCenter},
                                              {"justify", CssKeyword::kJustify},
                                              {"start", CssKeyword::kStart},
                                              {"end", CssKeyword::kEnd}}};
constexpr std::array<Word, 4> kDecorations = {{{"none", CssKeyword::kNone},
                                               {"underline", CssKeyword::kUnderline},
                                               {"line-through", CssKeyword::kLineThrough},
                                               {"overline", CssKeyword::kOverline}}};
constexpr std::array<Word, 8> kVerticalAligns = {{{"baseline", CssKeyword::kBaseline},
                                                  {"sub", CssKeyword::kSub},
                                                  {"super", CssKeyword::kSuper},
                                                  {"top", CssKeyword::kTop},
                                                  {"bottom", CssKeyword::kBottom},
                                                  {"middle", CssKeyword::kMiddle},
                                                  {"text-top", CssKeyword::kTextTop},
                                                  {"text-bottom", CssKeyword::kTextBottom}}};
constexpr std::array<Word, 5> kDisplays = {{{"block", CssKeyword::kBlock},
                                            {"inline", CssKeyword::kInline},
                                            {"inline-block", CssKeyword::kInlineBlock},
                                            {"list-item", CssKeyword::kListItem},
                                            {"none", CssKeyword::kNone}}};
constexpr std::array<Word, 3> kFloats = {
    {{"left", CssKeyword::kLeft}, {"right", CssKeyword::kRight}, {"none", CssKeyword::kNone}}};
// Where a page may be made to end, or kept from ending. A break to a left
// or a right page is a break; breaks between columns are not read.
constexpr std::array<Word, 9> kBreaksAround = {{{"auto", CssKeyword::kAuto},
                                                {"always", CssKeyword::kAlways},
                                                {"avoid", CssKeyword::kAvoid},
                                                {"page", CssKeyword::kAlways},
                                                {"left", CssKeyword::kAlways},
                                                {"right", CssKeyword::kAlways},
                                                {"recto", CssKeyword::kAlways},
                                                {"verso", CssKeyword::kAlways},
                                                {"avoid-page", CssKeyword::kAvoid}}};
constexpr std::array<Word, 3> kBreaksInside = {
    {{"auto", CssKeyword::kAuto}, {"avoid", CssKeyword::kAvoid}, {"avoid-page", CssKeyword::kAvoid}}};

CssValue KeywordValue(CssKeyword keyword) noexcept {
  CssValue value;
  value.kind = CssValue::Kind::kKeyword;
  value.keyword = keyword;
  return value;
}

CssValue FamiliesValue(std::string_view families) noexcept {
  CssValue value;
  value.kind = CssValue::Kind::kFamilies;
  value.families = families;
  value.familyClass = CssClassOfFamilies(families);
  return value;
}

bool FindWord(std::span<const Word> words, std::string_view text, CssValue& value) noexcept {
  for (const Word& word : words) {
    if (SameButForCase(text, word.name)) {
      value = KeywordValue(word.keyword);
      return true;
    }
  }
  return false;
}

// What a unit is in the units that are kept: one of it is `times` over
// `over` of `unit`.
struct UnitName {
  std::string_view name;
  CssUnit unit;
  std::int32_t times;
  std::int32_t over;
};

constexpr std::array<UnitName, 12> kUnits = {{
    {"px", CssUnit::kPx, 1, 1},
    {"em", CssUnit::kEm, 1, 1},
    {"rem", CssUnit::kRem, 1, 1},
    {"%", CssUnit::kPercent, 1, 1},
    {"pt", CssUnit::kPx, 4, 3},  // 96 pixels to the inch, and 72 points.
    {"pc", CssUnit::kPx, 16, 1},
    {"in", CssUnit::kPx, 96, 1},
    {"cm", CssUnit::kPx, 9600, 254},
    {"mm", CssUnit::kPx, 960, 254},
    {"q", CssUnit::kPx, 240, 254},
    {"ex", CssUnit::kEm, 1, 2},  // Near enough, for a face that is not known.
    {"ch", CssUnit::kEm, 1, 2},
}};

// What a length may be.
struct Lengths {
  bool negative = false;  // Below zero.
  bool number = false;    // A number with no unit, other than 0.
  bool percent = true;
};

// A number with its unit, or a number alone: "1.5em", "-3px", "120%", "0".
bool ParseLength(std::string_view text, Lengths may, CssValue& value) noexcept {
  std::size_t at = 0;
  bool below = false;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    below = text[at] == '-';
    ++at;
  }
  std::int64_t whole = 0;
  std::int64_t thousandths = 0;
  bool digits = false;
  for (; at < text.size() && IsDigit(text[at]); ++at) {
    whole = std::min<std::int64_t>((whole * 10) + (text[at] - '0'), kCssMaxNumber);
    digits = true;
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    std::int64_t scale = 100;
    bool fraction = false;
    for (; at < text.size() && IsDigit(text[at]); ++at) {
      thousandths += scale * (text[at] - '0');
      scale /= 10;
      fraction = true;
    }
    if (!fraction) {
      return false;  // "1." is not a number.
    }
    digits = true;
  }
  if (!digits) {
    return false;
  }
  const std::string_view unitName = After(text, at);
  if (unitName.size() > 1 && (unitName[0] == 'e' || unitName[0] == 'E') &&
      (IsDigit(unitName[1]) || unitName[1] == '+' || unitName[1] == '-')) {
    return false;  // A number written with a power of ten: no book has one.
  }
  std::int64_t number = (whole * 1000) + thousandths;
  CssUnit unit = CssUnit::kNumber;
  if (unitName.empty()) {
    if (number != 0 && !may.number) {
      return false;
    }
    // Nothing of any unit is nothing of every unit.
    unit = may.number ? CssUnit::kNumber : CssUnit::kPx;
  } else {
    const UnitName* found = nullptr;
    for (const UnitName& known : kUnits) {
      if (SameButForCase(unitName, known.name)) {
        found = &known;
      }
    }
    if (found == nullptr || (found->unit == CssUnit::kPercent && !may.percent)) {
      return false;
    }
    unit = found->unit;
    // To the nearest thousandth.
    number = ((std::min<std::int64_t>(number, kCssMaxNumber) * found->times) + (found->over / 2)) / found->over;
  }
  number = std::min<std::int64_t>(number, kCssMaxNumber);
  if (below) {
    if (!may.negative && number != 0) {
      return false;
    }
    number = -number;
  }
  value = CssValue();
  value.kind = CssValue::Kind::kLength;
  value.number = static_cast<std::int32_t>(number);
  value.unit = unit;
  return true;
}

// The next piece of a value, where pieces have spaces between them:
// brackets and quotes keep their spaces. `rest` is moved past it.
std::string_view NextPiece(std::string_view& rest) noexcept {
  rest = Trimmed(rest);
  std::size_t at = 0;
  std::uint32_t depth = 0;
  char quote = 0;
  for (; at < rest.size(); ++at) {
    const char ch = rest[at];
    if (quote != 0) {
      if (ch == '\\' && at + 1 < rest.size()) {
        ++at;
      } else if (ch == quote) {
        quote = 0;
      }
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      depth -= depth > 0 ? 1 : 0;
    } else if (depth == 0 && IsSpace(ch)) {
      break;
    }
  }
  const std::string_view piece = First(rest, at);
  rest = After(rest, at);
  return piece;
}

// Reads one declaration's value for a property and tells the sink.
class Declaring {
 public:
  Declaring(const Sink& sink, bool important) noexcept : sink_(sink), important_(important) {}

  // A property whose value is one of some words.
  bool Words(CssProperty property, std::span<const Word> words, std::string_view text) const noexcept {
    CssValue value;
    if (!Everywhere(text, value) && !FindWord(words, text, value)) {
      return false;
    }
    sink_.Declare(property, value, important_);
    return true;
  }

  // One whose value is a length, or one of some words.
  bool Length(CssProperty property, Lengths may, std::span<const Word> words, std::string_view text) const noexcept {
    CssValue value;
    if (!Everywhere(text, value) && !FindWord(words, text, value) && !ParseLength(text, may, value)) {
      return false;
    }
    sink_.Declare(property, value, important_);
    return true;
  }

  bool FontWeight(std::string_view text) const noexcept {
    CssValue value;
    if (!Everywhere(text, value) && !FindWord(kFontWeights, text, value)) {
      // A number from 1 to 1000, with nothing after the point.
      if (!ParseLength(text, {.number = true, .percent = false}, value) || value.unit != CssUnit::kNumber ||
          value.number < 1000 || value.number > 1'000'000 || value.number % 1000 != 0) {
        return false;
      }
    }
    sink_.Declare(CssProperty::kFontWeight, value, important_);
    return true;
  }

  bool FontFamily(std::string_view text) const noexcept {
    CssValue value;
    if (text.empty()) {
      return false;
    }
    if (!Everywhere(text, value)) {
      value = FamiliesValue(text);
    }
    sink_.Declare(CssProperty::kFontFamily, value, important_);
    return true;
  }

  // The lines drawn on text. Of several, the first is the one; a colour
  // or a kind of line beside it is passed over.
  bool TextDecoration(std::string_view text) const noexcept {
    CssValue value;
    if (!Everywhere(text, value)) {
      bool found = false;
      for (std::string_view piece = NextPiece(text); !piece.empty() && !found; piece = NextPiece(text)) {
        found = FindWord(kDecorations, piece, value);
      }
      if (!found) {
        return false;
      }
    }
    sink_.Declare(CssProperty::kTextDecoration, value, important_);
    return true;
  }

  // `margin` and `padding`: one to four lengths, for the four sides as
  // CSS deals them out.
  bool Sides(CssProperty top, Lengths may, std::span<const Word> words, std::string_view text) const noexcept {
    std::array<CssValue, 4> values;
    std::size_t count = 0;
    for (std::string_view piece = NextPiece(text); !piece.empty(); piece = NextPiece(text)) {
      if (count == values.size()) {
        return false;
      }
      CssValue& value = values[count];
      const bool everywhere = Everywhere(piece, value);
      if (everywhere && (count != 0 || !Trimmed(text).empty())) {
        return false;  // `inherit` is the whole value or no part of it.
      }
      if (!everywhere && !FindWord(words, piece, value) && !ParseLength(piece, may, value)) {
        return false;
      }
      ++count;
    }
    if (count == 0) {
      return false;
    }
    // Top, right, bottom, left: which of the values given each one takes.
    constexpr std::array<std::array<std::size_t, 4>, 4> kWhich = {
        {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}}};
    for (std::size_t side = 0; side < 4; ++side) {
      const auto property = static_cast<CssProperty>(static_cast<std::size_t>(top) + side);
      sink_.Declare(property, values[kWhich[count - 1][side]], important_);
    }
    return true;
  }

  // `font`: style, variant and weight in any order, then the size, with
  // the line's height after a `/` if it is given, then the family. What
  // is left out is set to normal, as CSS has it.
  bool Font(std::string_view text) const noexcept {
    CssValue all;
    if (Everywhere(text, all)) {
      for (const CssProperty property : {CssProperty::kFontStyle, CssProperty::kFontVariant, CssProperty::kFontWeight,
                                         CssProperty::kFontSize, CssProperty::kLineHeight, CssProperty::kFontFamily}) {
        sink_.Declare(property, all, important_);
      }
      return true;
    }
    const CssValue normal = KeywordValue(CssKeyword::kNormal);
    CssValue style = normal;
    CssValue variant = normal;
    CssValue weight = normal;
    CssValue size;
    CssValue lineHeight = normal;
    std::string_view piece = NextPiece(text);
    for (int before = 0; before < 4 && !piece.empty(); ++before) {
      CssValue value;
      if (SameButForCase(piece, "normal")) {
        // Which of the three it is normal for makes no difference.
      } else if (FindWord(kFontStyles, piece, value)) {
        style = value;
      } else if (FindWord(kFontVariants, piece, value)) {
        variant = value;
      } else if (FindWord(kFontWeights, piece, value) ||
                 (ParseLength(piece, {.number = true, .percent = false}, value) && value.unit == CssUnit::kNumber &&
                  value.number >= 1000 && value.number <= 1'000'000 && value.number % 1000 == 0)) {
        weight = value;
      } else {
        break;
      }
      piece = NextPiece(text);
    }
    // The size, and after a slash the line's height.
    const std::size_t slash = piece.find('/');
    const std::string_view sizeText = First(piece, slash);
    if (!FindWord(kFontSizes, sizeText, size) && !ParseLength(sizeText, {}, size)) {
      return false;
    }
    if (slash != std::string_view::npos) {
      const std::string_view heightText = After(piece, slash + 1);
      if (!FindWord(kNormalOnly, heightText, lineHeight) && !ParseLength(heightText, {.number = true}, lineHeight)) {
        return false;
      }
    }
    const std::string_view family = Trimmed(text);
    if (family.empty()) {
      return false;
    }
    sink_.Declare(CssProperty::kFontStyle, style, important_);
    sink_.Declare(CssProperty::kFontVariant, variant, important_);
    sink_.Declare(CssProperty::kFontWeight, weight, important_);
    sink_.Declare(CssProperty::kFontSize, size, important_);
    sink_.Declare(CssProperty::kLineHeight, lineHeight, important_);
    sink_.Declare(CssProperty::kFontFamily, FamiliesValue(family), important_);
    return true;
  }

 private:
  static bool Everywhere(std::string_view text, CssValue& value) noexcept { return FindWord(kEverywhere, text, value); }

  const Sink& sink_;
  bool important_;
};

// Takes `!important` off the end of a value, if it is there.
bool TakeImportant(std::string_view& value) noexcept {
  constexpr std::string_view kWord = "important";
  std::string_view text = Trimmed(value);
  if (text.size() < kWord.size() + 1 || !SameButForCase(After(text, text.size() - kWord.size()), kWord)) {
    return false;
  }
  text.remove_suffix(kWord.size());
  text = Trimmed(text);
  if (text.empty() || text.back() != '!') {
    return false;
  }
  text.remove_suffix(1);
  value = Trimmed(text);
  return true;
}

// One declaration, "name: value", told to the sink or dropped.
void Declare(const Sink& sink, std::string_view text) noexcept {
  text = Trimmed(text);
  if (text.empty()) {
    return;
  }
  const std::size_t colon = text.find(':');
  const std::string_view name = Trimmed(First(text, colon));
  if (colon == std::string_view::npos || name.empty() || !std::all_of(name.begin(), name.end(), IsNameByte)) {
    sink.Drop(CssDropped::kDeclaration, text);
    return;
  }
  std::string_view value = Trimmed(After(text, colon + 1));
  const Declaring declaring(sink, TakeImportant(value));
  const auto is = [name](std::string_view property) { return SameButForCase(name, property); };
  const Lengths anyLength = {.negative = true};
  const Lengths notBelowZero = {};
  bool known = true;
  bool understood = false;
  if (is("font-style")) {
    understood = declaring.Words(CssProperty::kFontStyle, kFontStyles, value);
  } else if (is("font-weight")) {
    understood = declaring.FontWeight(value);
  } else if (is("font-variant")) {
    understood = declaring.Words(CssProperty::kFontVariant, kFontVariants, value);
  } else if (is("font-family")) {
    understood = declaring.FontFamily(value);
  } else if (is("font-size")) {
    understood = declaring.Length(CssProperty::kFontSize, notBelowZero, kFontSizes, value);
  } else if (is("font")) {
    understood = declaring.Font(value);
  } else if (is("line-height")) {
    understood = declaring.Length(CssProperty::kLineHeight, {.number = true}, kNormalOnly, value);
  } else if (is("text-align")) {
    understood = declaring.Words(CssProperty::kTextAlign, kTextAligns, value);
  } else if (is("text-indent")) {
    understood = declaring.Length(CssProperty::kTextIndent, anyLength, {}, value);
  } else if (is("text-decoration") || is("text-decoration-line")) {
    understood = declaring.TextDecoration(value);
  } else if (is("vertical-align")) {
    understood = declaring.Length(CssProperty::kVerticalAlign, anyLength, kVerticalAligns, value);
  } else if (is("margin")) {
    understood = declaring.Sides(CssProperty::kMarginTop, anyLength, kAutoOnly, value);
  } else if (is("margin-top")) {
    understood = declaring.Length(CssProperty::kMarginTop, anyLength, kAutoOnly, value);
  } else if (is("margin-right")) {
    understood = declaring.Length(CssProperty::kMarginRight, anyLength, kAutoOnly, value);
  } else if (is("margin-bottom")) {
    understood = declaring.Length(CssProperty::kMarginBottom, anyLength, kAutoOnly, value);
  } else if (is("margin-left")) {
    understood = declaring.Length(CssProperty::kMarginLeft, anyLength, kAutoOnly, value);
  } else if (is("padding")) {
    understood = declaring.Sides(CssProperty::kPaddingTop, notBelowZero, {}, value);
  } else if (is("padding-top")) {
    understood = declaring.Length(CssProperty::kPaddingTop, notBelowZero, {}, value);
  } else if (is("padding-right")) {
    understood = declaring.Length(CssProperty::kPaddingRight, notBelowZero, {}, value);
  } else if (is("padding-bottom")) {
    understood = declaring.Length(CssProperty::kPaddingBottom, notBelowZero, {}, value);
  } else if (is("padding-left")) {
    understood = declaring.Length(CssProperty::kPaddingLeft, notBelowZero, {}, value);
  } else if (is("display")) {
    understood = declaring.Words(CssProperty::kDisplay, kDisplays, value);
  } else if (is("float")) {
    understood = declaring.Words(CssProperty::kFloat, kFloats, value);
  } else if (is("page-break-before") || is("break-before")) {
    understood = declaring.Words(CssProperty::kBreakBefore, kBreaksAround, value);
  } else if (is("page-break-after") || is("break-after")) {
    understood = declaring.Words(CssProperty::kBreakAfter, kBreaksAround, value);
  } else if (is("page-break-inside") || is("break-inside")) {
    understood = declaring.Words(CssProperty::kBreakInside, kBreaksInside, value);
  } else if (is("width")) {
    understood = declaring.Length(CssProperty::kWidth, notBelowZero, kAutoOnly, value);
  } else if (is("height")) {
    understood = declaring.Length(CssProperty::kHeight, notBelowZero, kAutoOnly, value);
  } else {
    known = false;
  }
  if (!known) {
    sink.Drop(CssDropped::kProperty, name);
  } else if (!understood) {
    sink.Drop(CssDropped::kValue, text);
  }
}

// Declarations with `;` between them, where a `;` inside brackets or
// quotes is not between two.
void DeclareAll(const Sink& sink, std::string_view text) noexcept {
  std::size_t start = 0;
  std::uint32_t depth = 0;
  char quote = 0;
  for (std::size_t at = 0; at <= text.size(); ++at) {
    const char ch = at < text.size() ? text[at] : ';';
    if (at < text.size() && quote != 0) {
      if (ch == '\\') {
        ++at;
      } else if (ch == quote) {
        quote = 0;
      }
    } else if (at < text.size() && (ch == '"' || ch == '\'')) {
      quote = ch;
    } else if (at < text.size() && ch == '(') {
      ++depth;
    } else if (at < text.size() && ch == ')') {
      depth -= depth > 0 ? 1 : 0;
    } else if (ch == ';' && (depth == 0 || at >= text.size())) {
      Declare(sink, First(After(text, start), std::min(at, text.size()) - start));
      start = at + 1;
    }
  }
}

// ---- Selectors ----

// Reads what a backslash stands for: `at` is at what follows the
// backslash, and is moved past it; the character it stands for is written
// at `out`, which is moved past that.
void TakeEscape(std::span<char> text, std::size_t& at, std::size_t& out) noexcept {
  if (!IsHex(text[at])) {
    text[out++] = text[at++];
    return;
  }
  // Up to six digits of a character's number, and one space after.
  std::uint32_t code = 0;
  for (int digits = 0; digits < 6 && at < text.size() && IsHex(text[at]); ++digits, ++at) {
    const char digit = AsciiLower(text[at]);
    code = (code * 16) + static_cast<std::uint32_t>(IsDigit(digit) ? digit - '0' : digit - 'a' + 10);
  }
  if (at < text.size() && IsSpace(text[at])) {
    ++at;
  }
  if (code == 0 || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
    // No character has that number: the mark for one that is missing
    // stands for it. The mark is three bytes, and "\0" is two: the text is
    // written where it is read from, and must not get ahead of the
    // reading, so there a `?` has to do.
    code = at - out >= 3 ? 0xFFFDU : '?';
  }
  // As UTF-8, which for a character that there is, is never longer than
  // the digits it was written with and their backslash.
  if (code < 0x80U) {
    text[out++] = static_cast<char>(code);
  } else if (code < 0x800U) {
    text[out++] = static_cast<char>(0xC0U | (code >> 6U));
    text[out++] = static_cast<char>(0x80U | (code & 0x3FU));
  } else if (code < 0x10000U) {
    text[out++] = static_cast<char>(0xE0U | (code >> 12U));
    text[out++] = static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    text[out++] = static_cast<char>(0x80U | (code & 0x3FU));
  } else {
    text[out++] = static_cast<char>(0xF0U | (code >> 18U));
    text[out++] = static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
    text[out++] = static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
    text[out++] = static_cast<char>(0x80U | (code & 0x3FU));
  }
}

// Reads a name where `at` is, taking the backslashes out of it where it
// stands, and moves `at` past it. Empty if there is no name there.
std::string_view TakeName(std::span<char> text, std::size_t& at) noexcept {
  const std::size_t start = at;
  std::size_t out = at;
  // A name begins with a letter, `_` or a byte past ASCII, after one `-`
  // or none, or with two of `-`.
  std::size_t first = at;
  if (first < text.size() && text[first] == '-') {
    ++first;
  }
  if (first >= text.size() || IsDigit(text[first]) || (!IsNameByte(text[first]) && text[first] != '\\') ||
      (text[first] == '-' && first == at)) {
    return {};
  }
  while (at < text.size()) {
    const char ch = text[at];
    if (IsNameByte(ch)) {
      text[out++] = ch;
      ++at;
    } else if (ch == '\\' && at + 1 < text.size() && text[at + 1] != '\n') {
      ++at;
      TakeEscape(text, at, out);
    } else {
      break;
    }
  }
  return {text.data() + start, out - start};
}

// Reads a string where `at` is, at its opening quote, taking its quotes
// and backslashes out of it where it stands, and moves `at` past it. False
// if it is not closed before its line or the text ends.
bool TakeString(std::span<char> text, std::size_t& at, std::string_view& string) noexcept {
  const char quote = text[at++];
  const std::size_t start = at;
  std::size_t out = at;
  while (at < text.size()) {
    const char ch = text[at];
    if (ch == quote) {
      ++at;
      string = {text.data() + start, out - start};
      return true;
    }
    if (ch == '\n') {
      return false;
    }
    if (ch != '\\') {
      text[out++] = ch;
      ++at;
    } else if (at + 1 >= text.size()) {
      return false;
    } else if (text[at + 1] == '\n') {
      at += 2;  // A line's end after a backslash is not part of the string.
    } else {
      ++at;
      TakeEscape(text, at, out);
    }
  }
  return false;
}

void LowerInPlace(std::span<char> text) noexcept {
  for (char& ch : text) {
    ch = AsciiLower(ch);
  }
}

void SkipSpaces(std::span<const char> text, std::size_t& at) noexcept {
  while (at < text.size() && IsSpace(text[at])) {
    ++at;
  }
}

// Reads what a selector asks of an attribute's value, after the
// attribute's name, as far as the `]`, and moves `at` past that. False if
// what is there is not that.
bool TakeAttributeValue(std::span<char> text, std::size_t& at, CssAttribute& attribute) noexcept {
  SkipSpaces(text, at);
  if (at < text.size() && text[at] == ']') {
    ++at;
    return true;
  }

  // What is asked, then the value: a string or a name.
  struct Asking {
    char mark;
    CssAttributeTest test;
  };
  constexpr std::array<Asking, 5> kAskings = {{{'~', CssAttributeTest::kWord},
                                               {'|', CssAttributeTest::kDash},
                                               {'^', CssAttributeTest::kBegins},
                                               {'$', CssAttributeTest::kEnds},
                                               {'*', CssAttributeTest::kContains}}};
  if (at < text.size() && text[at] == '=') {
    attribute.test = CssAttributeTest::kEquals;
    ++at;
  } else {
    const auto* asking = std::find_if(kAskings.begin(), kAskings.end(), [&text, at](const Asking& one) {
      return at + 1 < text.size() && text[at] == one.mark && text[at + 1] == '=';
    });
    if (asking == kAskings.end()) {
      return false;
    }
    attribute.test = asking->test;
    at += 2;
  }
  SkipSpaces(text, at);
  if (at < text.size() && (text[at] == '"' || text[at] == '\'')) {
    if (!TakeString(text, at, attribute.value)) {
      return false;
    }
  } else {
    attribute.value = TakeName(text, at);
    if (attribute.value.empty()) {
      return false;
    }
  }

  // After the value there may be an `i`, for any case, or an `s`, for the
  // case as written, which it is without one.
  SkipSpaces(text, at);
  if (at < text.size() && text[at] != ']') {
    const std::string_view flag = TakeName(text, at);
    if (flag.size() != 1 || (AsciiLower(flag.front()) != 'i' && AsciiLower(flag.front()) != 's')) {
      return false;
    }
    attribute.anyCase = AsciiLower(flag.front()) == 'i';
    SkipSpaces(text, at);
  }
  if (at >= text.size() || text[at] != ']') {
    return false;
  }
  ++at;
  return true;
}

// Reads what a selector asks of an attribute, where `at` is at its `[`,
// and moves `at` past its `]`. False if what is there is not that.
bool TakeAttribute(std::span<char> text, std::size_t& at, CssAttribute& attribute) noexcept {
  attribute = CssAttribute();
  ++at;
  SkipSpaces(text, at);

  // The name, which may have a namespace's prefix and a `|` before it:
  // `*|` for any namespace, and `|` alone for none, which is what a name
  // without a prefix has anyway.
  bool mayHavePrefix = true;
  if (at + 1 < text.size() && text[at] == '*' && text[at + 1] == '|') {
    attribute.anyPrefix = true;
    mayHavePrefix = false;
    at += 2;
  } else if (at + 1 < text.size() && text[at] == '|' && text[at + 1] != '=') {
    mayHavePrefix = false;
    ++at;
  }
  const std::size_t nameStart = at;
  std::size_t nameLength = TakeName(text, at).size();
  if (nameLength == 0) {
    return false;
  }
  std::size_t localStart = 0;
  std::size_t localLength = 0;
  if (mayHavePrefix && at + 1 < text.size() && text[at] == '|' && text[at + 1] != '=') {
    // That was a prefix, and the name comes after it.
    localStart = ++at;
    localLength = TakeName(text, at).size();
    if (localLength == 0) {
      return false;
    }
  }
  if (!TakeAttributeValue(text, at, attribute)) {
    return false;
  }

  // Now that it is known to be one, the name is made what a document has:
  // in small letters, and after a prefix a `:` and the rest, which is
  // never further on than where it was read.
  if (localLength != 0) {
    text[nameStart + nameLength] = ':';
    for (std::size_t i = 0; i < localLength; ++i) {
      text[nameStart + nameLength + 1 + i] = text[localStart + i];
    }
    nameLength += 1 + localLength;
  }
  LowerInPlace(text.subspan(nameStart, nameLength));
  attribute.name = ViewOf(text.subspan(nameStart, nameLength));
  return true;
}

// Moves `at` past brackets that open where it is, to after the one that
// closes them, or to the end.
void SkipBrackets(std::span<const char> text, std::size_t& at) noexcept {
  std::uint32_t depth = 0;
  char quote = 0;
  for (; at < text.size(); ++at) {
    const char ch = text[at];
    if (quote != 0) {
      if (ch == '\\') {
        ++at;
      } else if (ch == quote) {
        quote = 0;
      }
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      if (--depth == 0) {
        ++at;
        return;
      }
    }
  }
}

// Reads one selector and tells the sink of it, or of why it was dropped.
// False if it was dropped.
bool ReadSelector(const Sink& sink, std::span<char> text) noexcept {
  text = Trimmed(text);
  const std::string_view whole = ViewOf(text);
  std::array<CssCompound, CssReader::kMaxCompounds> compounds;
  std::array<std::string_view, CssReader::kMaxClasses> classes;
  std::array<CssAttribute, CssReader::kMaxAttributes> attributes;
  // Where each part's classes begin among `classes`, and how many it has;
  // and the same for what it asks of attributes.
  std::array<std::size_t, CssReader::kMaxCompounds> classStart{};
  std::array<std::size_t, CssReader::kMaxCompounds> classCount{};
  std::array<std::size_t, CssReader::kMaxCompounds> attributeStart{};
  std::array<std::size_t, CssReader::kMaxCompounds> attributeCount{};
  std::size_t compoundCount = 0;
  std::size_t classTotal = 0;
  std::size_t attributeTotal = 0;
  std::uint32_t ids = 0;
  std::uint32_t classLike = 0;
  std::uint32_t elementLike = 0;
  bool firstLetter = false;
  CssCombinator combinator = CssCombinator::kFirst;
  bool wantPart = true;  // A part has to come next: the start, or after a `>` or a `+`.

  std::size_t at = 0;
  while (at < text.size()) {
    bool space = false;
    while (at < text.size() && IsSpace(text[at])) {
      ++at;
      space = true;
    }
    if (at >= text.size()) {
      break;
    }
    const char ch = text[at];
    if (ch == '~') {
      sink.Drop(CssDropped::kSelectorCombinator, whole);
      return false;
    }
    if (ch == '>' || ch == '+') {
      if (wantPart) {
        sink.Drop(CssDropped::kSelectorOther, whole);
        return false;
      }
      combinator = ch == '>' ? CssCombinator::kChild : CssCombinator::kAdjacent;
      wantPart = true;
      ++at;
      continue;
    }
    if (!wantPart) {
      if (!space) {
        sink.Drop(CssDropped::kSelectorOther, whole);
        return false;
      }
      combinator = CssCombinator::kDescendant;
    }
    if (firstLetter || compoundCount == compounds.size()) {
      // Nothing comes after `::first-letter`; and there is room for only
      // so many parts.
      sink.Drop(CssDropped::kSelectorOther, whole);
      return false;
    }

    // One part: an element's name or `*`, then classes, an id, what is
    // asked of attributes and pseudo-classes, in any order.
    CssCompound& compound = compounds[compoundCount];
    compound = CssCompound();
    compound.combinator = combinator;
    classStart[compoundCount] = classTotal;
    attributeStart[compoundCount] = attributeTotal;
    bool any = false;
    if (ch == '*') {
      any = true;
      ++at;
    } else if (ch != '.' && ch != '#' && ch != ':' && ch != '[') {
      const std::size_t start = at;
      const std::string_view element = TakeName(text, at);
      if (element.empty()) {
        sink.Drop(CssDropped::kSelectorOther, whole);
        return false;
      }
      LowerInPlace(text.subspan(start, element.size()));
      compound.element = element;
      ++elementLike;
      any = true;
    }
    if (at < text.size() && text[at] == '|') {
      sink.Drop(CssDropped::kSelectorOther, whole);  // A name with a namespace before it.
      return false;
    }
    while (at < text.size() && !IsSpace(text[at]) && text[at] != '>' && text[at] != '+' && text[at] != '~') {
      const char mark = text[at];
      if (firstLetter) {
        sink.Drop(CssDropped::kSelectorOther, whole);  // Nothing comes after `::first-letter`, in its own part either.
        return false;
      }
      if (mark == '[') {
        if (attributeTotal == attributes.size()) {
          sink.Drop(CssDropped::kSelectorOther, whole);
          return false;
        }
        if (!TakeAttribute(text, at, attributes[attributeTotal])) {
          sink.Drop(CssDropped::kSelectorAttribute, whole);
          return false;
        }
        ++attributeTotal;
        ++classLike;
        any = true;
        continue;
      }
      if (mark == '.' || mark == '#') {
        ++at;
        const std::string_view name = TakeName(text, at);
        if (name.empty() || (mark == '#' && !compound.id.empty()) || (mark == '.' && classTotal == classes.size())) {
          sink.Drop(CssDropped::kSelectorOther, whole);
          return false;
        }
        if (mark == '#') {
          compound.id = name;
          ++ids;
        } else {
          classes[classTotal++] = name;
          ++classLike;
        }
        any = true;
        continue;
      }
      if (mark != ':') {
        sink.Drop(CssDropped::kSelectorOther, whole);
        return false;
      }
      // A pseudo-class, with one colon, or a pseudo-element, with two; and
      // the four pseudo-elements that are older than the second colon.
      const std::size_t colon = at;
      ++at;
      bool element = false;
      if (at < text.size() && text[at] == ':') {
        element = true;
        ++at;
      }
      const std::size_t nameStart = at;
      const std::string_view name = TakeName(text, at);
      if (name.empty()) {
        sink.Drop(CssDropped::kSelectorOther, whole);
        return false;
      }
      LowerInPlace(text.subspan(nameStart, name.size()));
      if (at < text.size() && text[at] == '(') {
        SkipBrackets(text, at);
      }
      element = element || name == "first-letter" || name == "first-line" || name == "before" || name == "after";
      if (element && name == "first-letter") {
        firstLetter = true;
        ++elementLike;
      } else if (!element && name == "link") {
        ++classLike;
      } else {
        sink.Drop(element ? CssDropped::kSelectorPseudoElement : CssDropped::kSelectorPseudoClass,
                  ViewOf(text.subspan(colon, nameStart + name.size() - colon)));
        return false;
      }
      any = true;
    }
    if (!any) {
      sink.Drop(CssDropped::kSelectorOther, whole);
      return false;
    }
    classCount[compoundCount] = classTotal - classStart[compoundCount];
    attributeCount[compoundCount] = attributeTotal - attributeStart[compoundCount];
    ++compoundCount;
    wantPart = false;
  }
  if (compoundCount == 0 || wantPart) {
    sink.Drop(CssDropped::kSelectorOther, whole);
    return false;
  }
  for (std::size_t i = 0; i < compoundCount; ++i) {
    compounds[i].classes = std::span<const std::string_view>(classes).subspan(classStart[i], classCount[i]);
    compounds[i].attributes = std::span<const CssAttribute>(attributes).subspan(attributeStart[i], attributeCount[i]);
  }
  const CssSelector selector = {.compounds = std::span<const CssCompound>(compounds).first(compoundCount),
                                .firstLetter = firstLetter,
                                .specificity = (std::min<std::uint32_t>(ids, 255) << 16U) |
                                               (std::min<std::uint32_t>(classLike, 255) << 8U) |
                                               std::min<std::uint32_t>(elementLike, 255)};
  sink.handler.OnSelector(selector);
  return true;
}

// Where the next selector of a list ends: at a comma that is not inside
// brackets or quotes, or at the end.
std::size_t SelectorEnd(std::span<const char> text) noexcept {
  std::uint32_t depth = 0;
  char quote = 0;
  for (std::size_t at = 0; at < text.size(); ++at) {
    const char ch = text[at];
    if (quote != 0) {
      if (ch == '\\') {
        ++at;
      } else if (ch == quote) {
        quote = 0;
      }
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      depth -= depth > 0 ? 1 : 0;
    } else if (ch == ',' && depth == 0) {
      return at;
    }
  }
  return text.size();
}

// What a url is, of `url(x)`, `url("x")` or `"x"`, with what follows it
// left in `after`. Empty if `text` is none of those.
std::string_view TakeUrl(std::string_view text, std::string_view& after) noexcept {
  text = Trimmed(text);
  if (text.size() >= 4 && SameButForCase(First(text, 4), "url(")) {
    const std::size_t close = text.find(')');
    if (close == std::string_view::npos) {
      return {};
    }
    after = After(text, close + 1);
    text = Trimmed(First(After(text, 4), close - 4));
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
      text = First(After(text, 1), text.size() - 2);
    }
    return text;
  }
  if (!text.empty() && (text.front() == '"' || text.front() == '\'')) {
    const std::size_t close = text.find(text.front(), 1);
    if (close == std::string_view::npos) {
      return {};
    }
    after = After(text, close + 1);
    return First(After(text, 1), close - 1);
  }
  return {};
}

// The text of a prelude without what a sheet inside an HTML page may have
// round it (`<!--` and `-->`), or the mark that some files begin with:
// none of them is part of the first selector.
std::span<char> Unwrapped(std::span<char> text) noexcept {
  text = Trimmed(text);
  for (const std::string_view wrapper :
       {std::string_view("\xEF\xBB\xBF"), std::string_view("<!--"), std::string_view("-->")}) {
    if (ViewOf(text).starts_with(wrapper)) {
      text = Trimmed(text.subspan(wrapper.size()));
    }
  }
  return text;
}

}  // namespace

// What the header says of its size.
static_assert(sizeof(CssReader) < 2048);

const char* CssPropertyName(CssProperty property) noexcept {
  switch (property) {
    case CssProperty::kFontStyle:
      return "font-style";
    case CssProperty::kFontWeight:
      return "font-weight";
    case CssProperty::kFontVariant:
      return "font-variant";
    case CssProperty::kFontFamily:
      return "font-family";
    case CssProperty::kFontSize:
      return "font-size";
    case CssProperty::kLineHeight:
      return "line-height";
    case CssProperty::kTextAlign:
      return "text-align";
    case CssProperty::kTextIndent:
      return "text-indent";
    case CssProperty::kTextDecoration:
      return "text-decoration";
    case CssProperty::kVerticalAlign:
      return "vertical-align";
    case CssProperty::kMarginTop:
      return "margin-top";
    case CssProperty::kMarginRight:
      return "margin-right";
    case CssProperty::kMarginBottom:
      return "margin-bottom";
    case CssProperty::kMarginLeft:
      return "margin-left";
    case CssProperty::kPaddingTop:
      return "padding-top";
    case CssProperty::kPaddingRight:
      return "padding-right";
    case CssProperty::kPaddingBottom:
      return "padding-bottom";
    case CssProperty::kPaddingLeft:
      return "padding-left";
    case CssProperty::kDisplay:
      return "display";
    case CssProperty::kFloat:
      return "float";
    case CssProperty::kBreakBefore:
      return "break-before";
    case CssProperty::kBreakAfter:
      return "break-after";
    case CssProperty::kBreakInside:
      return "break-inside";
    case CssProperty::kWidth:
      return "width";
    case CssProperty::kHeight:
      return "height";
  }
  return "unknown";
}

const char* CssKeywordName(CssKeyword keyword) noexcept {
  switch (keyword) {
    case CssKeyword::kNone:
      return "none";
    case CssKeyword::kInherit:
      return "inherit";
    case CssKeyword::kInitial:
      return "initial";
    case CssKeyword::kNormal:
      return "normal";
    case CssKeyword::kAuto:
      return "auto";
    case CssKeyword::kItalic:
      return "italic";
    case CssKeyword::kOblique:
      return "oblique";
    case CssKeyword::kBold:
      return "bold";
    case CssKeyword::kBolder:
      return "bolder";
    case CssKeyword::kLighter:
      return "lighter";
    case CssKeyword::kSmallCaps:
      return "small-caps";
    case CssKeyword::kLeft:
      return "left";
    case CssKeyword::kRight:
      return "right";
    case CssKeyword::kCenter:
      return "center";
    case CssKeyword::kJustify:
      return "justify";
    case CssKeyword::kStart:
      return "start";
    case CssKeyword::kEnd:
      return "end";
    case CssKeyword::kUnderline:
      return "underline";
    case CssKeyword::kLineThrough:
      return "line-through";
    case CssKeyword::kOverline:
      return "overline";
    case CssKeyword::kBaseline:
      return "baseline";
    case CssKeyword::kSub:
      return "sub";
    case CssKeyword::kSuper:
      return "super";
    case CssKeyword::kTop:
      return "top";
    case CssKeyword::kBottom:
      return "bottom";
    case CssKeyword::kMiddle:
      return "middle";
    case CssKeyword::kTextTop:
      return "text-top";
    case CssKeyword::kTextBottom:
      return "text-bottom";
    case CssKeyword::kBlock:
      return "block";
    case CssKeyword::kInline:
      return "inline";
    case CssKeyword::kInlineBlock:
      return "inline-block";
    case CssKeyword::kListItem:
      return "list-item";
    case CssKeyword::kAlways:
      return "always";
    case CssKeyword::kAvoid:
      return "avoid";
    case CssKeyword::kXxSmall:
      return "xx-small";
    case CssKeyword::kXSmall:
      return "x-small";
    case CssKeyword::kSmall:
      return "small";
    case CssKeyword::kMedium:
      return "medium";
    case CssKeyword::kLarge:
      return "large";
    case CssKeyword::kXLarge:
      return "x-large";
    case CssKeyword::kXxLarge:
      return "xx-large";
    case CssKeyword::kSmaller:
      return "smaller";
    case CssKeyword::kLarger:
      return "larger";
  }
  return "unknown";
}

const char* CssUnitName(CssUnit unit) noexcept {
  switch (unit) {
    case CssUnit::kNumber:
      return "";
    case CssUnit::kPx:
      return "px";
    case CssUnit::kEm:
      return "em";
    case CssUnit::kRem:
      return "rem";
    case CssUnit::kPercent:
      return "%";
  }
  return "?";
}

const char* CssAttributeTestName(CssAttributeTest test) noexcept {
  switch (test) {
    case CssAttributeTest::kPresent:
      return "";
    case CssAttributeTest::kEquals:
      return "=";
    case CssAttributeTest::kWord:
      return "~=";
    case CssAttributeTest::kDash:
      return "|=";
    case CssAttributeTest::kBegins:
      return "^=";
    case CssAttributeTest::kEnds:
      return "$=";
    case CssAttributeTest::kContains:
      return "*=";
  }
  return "?";
}

const char* CssDroppedName(CssDropped dropped) noexcept {
  switch (dropped) {
    case CssDropped::kSelectorAttribute:
      return "attribute selector";
    case CssDropped::kSelectorPseudoClass:
      return "pseudo-class";
    case CssDropped::kSelectorPseudoElement:
      return "pseudo-element";
    case CssDropped::kSelectorCombinator:
      return "sibling selector";
    case CssDropped::kSelectorOther:
      return "selector";
    case CssDropped::kRule:
      return "rule";
    case CssDropped::kProperty:
      return "property";
    case CssDropped::kValue:
      return "value";
    case CssDropped::kDeclaration:
      return "declaration";
    case CssDropped::kAtRule:
      return "at-rule";
    case CssDropped::kMedia:
      return "media block";
    case CssDropped::kTooLong:
      return "too long";
  }
  return "unknown";
}

const char* CssStatusName(CssStatus status) noexcept {
  switch (status) {
    case CssStatus::kOk:
      return "ok";
    case CssStatus::kNotStarted:
      return "not started";
    case CssStatus::kSourceFailed:
      return "source failed";
  }
  return "unknown";
}

bool CssMediaApplies(std::string_view query) noexcept {
  query = Trimmed(query);
  if (query.empty()) {
    return true;
  }
  // A list is for a reader if any query in it is.
  while (!query.empty()) {
    const std::size_t comma = query.find(',');
    std::string_view one = Trimmed(First(query, comma));
    query = comma == std::string_view::npos ? std::string_view() : After(query, comma + 1);
    // A query with a condition in brackets is not for it, whatever the
    // rest says: see the top of the header.
    if (one.empty() || one.find('(') != std::string_view::npos) {
      continue;
    }
    bool turned = false;
    std::string_view word = NextPiece(one);
    if (SameButForCase(word, "not")) {
      turned = true;
      word = NextPiece(one);
    } else if (SameButForCase(word, "only")) {
      word = NextPiece(one);
    }
    if (word.empty() || !Trimmed(one).empty()) {
      continue;  // Not a query that is understood.
    }
    const bool screen = SameButForCase(word, "all") || SameButForCase(word, "screen");
    if (screen != turned) {
      return true;
    }
  }
  return false;
}

std::string_view CssNextFamily(std::string_view& rest, std::span<char> name) noexcept {
  std::size_t length = 0;
  const auto add = [&name, &length](char ch) {
    if (length < name.size()) {
      name[length++] = AsciiLower(ch);
    }
  };
  while (!rest.empty() && length == 0) {
    // One name: up to a comma that is not inside quotes.
    char quote = 0;
    bool space = false;
    std::size_t at = 0;
    for (; at < rest.size(); ++at) {
      const char ch = rest[at];
      if (quote != 0) {
        if (ch == quote) {
          quote = 0;
          continue;
        }
        if (ch == '\\' && at + 1 < rest.size()) {
          ++at;
        }
      } else if (ch == '"' || ch == '\'') {
        quote = ch;
        continue;
      } else if (ch == ',') {
        break;
      }
      if (IsSpace(rest[at])) {
        space = length != 0;
        continue;
      }
      if (space) {
        add(' ');
        space = false;
      }
      add(rest[at]);
    }
    rest = After(rest, at + 1);
  }
  return {name.data(), length};
}

void ReadCssDeclarations(std::string_view text, CssHandler& handler) noexcept {
  DeclareAll(Sink{.handler = handler}, text);
}

// ---- A sheet ----

void CssReader::Begin(core::ByteSource& source, CssHandler& handler) noexcept {
  source_ = &source;
  handler_ = &handler;
  status_ = CssStatus::kOk;
  finished_ = false;
  place_ = Place::kPrelude;
  inComment_ = false;
  slash_ = false;
  star_ = false;
  quote_ = 0;
  escaped_ = false;
  ruleTold_ = false;
  skipDepth_ = 0;
  mediaDepth_ = 0;
  rules_ = 0;
  declarations_ = 0;
  dropped_ = {};
  ClearText();
  cssSheets.Add();
}

bool CssReader::Step() noexcept {
  if (source_ == nullptr || finished_ || status_ != CssStatus::kOk) {
    return false;
  }
  std::array<std::uint8_t, kChunkBytes> chunk;  // NOLINT(cppcoreguidelines-pro-type-member-init): filled by the read.
  const std::ptrdiff_t read = source_->Read(chunk);
  if (read < 0 || static_cast<std::size_t>(read) > chunk.size()) {
    status_ = CssStatus::kSourceFailed;
    return false;
  }
  if (read == 0) {
    End();
    finished_ = true;
    return false;
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(read); ++i) {
    Take(static_cast<char>(chunk[i]));
  }
  return true;
}

CssStatus CssReader::Read(core::ByteSource& source, CssHandler& handler) noexcept {
  Begin(source, handler);
  while (Step()) {
  }
  return status_;
}

void CssReader::Drop(CssDropped dropped, std::string_view text) noexcept {
  ++dropped_[static_cast<std::size_t>(dropped)];
  handler_->OnDropped(dropped, text);
}

void CssReader::ClearText() noexcept {
  length_ = 0;
  tooLong_ = false;
  parens_ = 0;
  braces_ = 0;
}

void CssReader::Keep(char ch) noexcept {
  const std::size_t room = place_ == Place::kDeclarations ? kMaxDeclaration : kMaxPrelude;
  if (length_ < room) {
    text_[length_++] = ch;
  } else {
    tooLong_ = true;
  }
}

// Comments are taken out here, wherever they are but inside quotes, so
// that nothing after this sees one.
void CssReader::Take(char ch) noexcept {
  if (inComment_) {
    if (star_ && ch == '/') {
      inComment_ = false;
    }
    star_ = ch == '*';
    return;
  }
  if (quote_ == 0 && !escaped_) {
    if (slash_) {
      slash_ = false;
      if (ch == '*') {
        inComment_ = true;
        star_ = false;
        return;
      }
      TakeText('/');
    }
    if (ch == '/') {
      slash_ = true;  // Held back until it is known what follows.
      return;
    }
  }
  TakeText(ch);
}

void CssReader::TakeText(char ch) noexcept {
  // Inside quotes, and after a backslash, a byte is only itself.
  if (quote_ != 0 || escaped_) {
    if (place_ != Place::kSkippedBlock) {
      Keep(ch);
    }
    if (escaped_) {
      escaped_ = false;
    } else if (ch == '\\') {
      escaped_ = true;
    } else if (ch == quote_ || ch == '\n') {
      quote_ = 0;  // A string ends at its quote, and a line's end ends one that was left open.
    }
    return;
  }
  if (ch == '"' || ch == '\'') {
    quote_ = ch;
  } else if (ch == '\\') {
    escaped_ = true;
  }
  switch (place_) {
    case Place::kPrelude:
      TakePrelude(ch);
      break;
    case Place::kDeclarations:
      TakeDeclarations(ch);
      break;
    case Place::kSkippedBlock:
      TakeSkipped(ch);
      break;
  }
}

void CssReader::TakePrelude(char ch) noexcept {
  if (length_ == 0 && IsSpace(ch)) {
    return;
  }
  if (ch == '{') {
    // A block begins here whatever brackets are open before it: a bracket
    // that was never closed is not to take the rest of the sheet with it.
    EndPrelude(true);
    return;
  }
  if (ch == '(' || ch == '[') {
    ++parens_;
  } else if ((ch == ')' || ch == ']') && parens_ > 0) {
    --parens_;
  } else if (parens_ == 0) {
    if (ch == ';' && IsAtRule()) {
      EndPrelude(false);
      return;
    }
    if (ch == '}') {
      // The end of an `@media` block that was being read; or a stray one,
      // which is passed over with whatever came before it.
      mediaDepth_ -= mediaDepth_ > 0 ? 1 : 0;
      ClearText();
      return;
    }
  }
  Keep(ch);
}

void CssReader::TakeDeclarations(char ch) noexcept {
  if (ch == '{') {
    ++braces_;
  } else if (ch == '}') {
    if (braces_ == 0) {
      EndDeclaration();
      EndRule();
      return;
    }
    --braces_;
  } else if (ch == '(' || ch == '[') {
    ++parens_;
  } else if ((ch == ')' || ch == ']') && parens_ > 0) {
    --parens_;
  } else if (ch == ';' && parens_ == 0 && braces_ == 0) {
    EndDeclaration();
    return;
  }
  Keep(ch);
}

void CssReader::TakeSkipped(char ch) noexcept {
  if (ch == '{') {
    ++skipDepth_;
  } else if (ch == '}' && --skipDepth_ == 0) {
    place_ = Place::kPrelude;
    ClearText();
  }
}

void CssReader::SkipBlock() noexcept {
  place_ = Place::kSkippedBlock;
  skipDepth_ = 1;
}

bool CssReader::IsAtRule() noexcept {
  const std::span<char> text = Unwrapped(std::span<char>(text_).first(length_));
  return !text.empty() && text.front() == '@';
}

void CssReader::EndPrelude(bool block) noexcept {
  const std::span<char> text = Unwrapped(std::span<char>(text_).first(length_));
  const bool tooLong = tooLong_;
  ClearText();
  if (tooLong) {
    Drop(CssDropped::kTooLong, ViewOf(text));
    if (block) {
      SkipBlock();
    }
    return;
  }
  if (!text.empty() && text.front() == '@') {
    EndAtRule(text, block);
    return;
  }
  if (!block) {
    return;
  }
  BeginRule(text);
}

void CssReader::EndAtRule(std::span<char> text, bool block) noexcept {
  std::size_t at = 1;
  const std::string_view name = TakeName(text, at);
  LowerInPlace(text.subspan(1, name.size()));
  const std::string_view rest = Trimmed(ViewOf(text.subspan(std::min(at, text.size()))));
  if (name == "media" && block) {
    if (CssMediaApplies(rest)) {
      ++mediaDepth_;  // Its rules are read as any others are, and its `}` closes it.
    } else {
      Drop(CssDropped::kMedia, rest);
      SkipBlock();
    }
    return;
  }
  if (name == "import" && !block) {
    std::string_view media;
    const std::string_view url = TakeUrl(rest, media);
    if (!url.empty() && CssMediaApplies(media)) {
      handler_->OnImport(url);
      return;
    }
  }
  Drop(CssDropped::kAtRule, name);
  if (block) {
    SkipBlock();
  }
}

void CssReader::BeginRule(std::span<char> selectors) noexcept {
  // The handler is told a rule has begun when its first selector that can
  // be read is found, and not before.
  class Announcing final : public CssHandler {
   public:
    explicit Announcing(CssHandler& handler) noexcept : handler_(handler) {}
    void OnRuleBegin() noexcept override {}
    void OnSelector(const CssSelector& selector) noexcept override {
      if (!told) {
        handler_.OnRuleBegin();
        told = true;
      }
      handler_.OnSelector(selector);
    }
    void OnDeclaration(const CssDeclaration& /*declaration*/) noexcept override {}
    void OnRuleEnd() noexcept override {}
    void OnImport(std::string_view /*url*/) noexcept override {}
    void OnDropped(CssDropped dropped, std::string_view text) noexcept override { handler_.OnDropped(dropped, text); }
    bool told = false;

   private:
    CssHandler& handler_;
  };
  Announcing announcing(*handler_);
  const Sink announcingSink = {.handler = announcing, .declarations = nullptr, .dropped = &dropped_};
  while (!selectors.empty()) {
    const std::size_t end = SelectorEnd(selectors);
    (void)ReadSelector(announcingSink, selectors.first(end));
    selectors = selectors.subspan(std::min(end + 1, selectors.size()));
  }
  ruleTold_ = announcing.told;
  if (!ruleTold_) {
    Drop(CssDropped::kRule, {});
    SkipBlock();
    return;
  }
  place_ = Place::kDeclarations;
}

void CssReader::EndDeclaration() noexcept {
  const std::string_view text = Trimmed(std::string_view(text_.data(), length_));
  const bool tooLong = tooLong_;
  ClearText();
  if (tooLong) {
    Drop(CssDropped::kTooLong, text);
    return;
  }
  Declare(Sink{.handler = *handler_, .declarations = &declarations_, .dropped = &dropped_}, text);
}

void CssReader::EndRule() noexcept {
  if (ruleTold_) {
    handler_->OnRuleEnd();
    ++rules_;
    cssRules.Add();
  }
  ruleTold_ = false;
  place_ = Place::kPrelude;
  ClearText();
}

void CssReader::End() noexcept {
  if (slash_) {
    slash_ = false;
    TakeText('/');
  }
  quote_ = 0;
  escaped_ = false;
  switch (place_) {
    case Place::kDeclarations:
      // A sheet that ends inside a rule ends the rule.
      EndDeclaration();
      EndRule();
      break;
    case Place::kPrelude:
      // An at-rule with nothing after it but the end is whole.
      if (IsAtRule()) {
        EndPrelude(false);
      }
      break;
    case Place::kSkippedBlock:
      break;
  }
  place_ = Place::kPrelude;
  ClearText();
}

}  // namespace bn::epub
