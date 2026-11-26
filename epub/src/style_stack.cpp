#include "epub/style_stack.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/xml.h"

namespace bn::epub {

namespace {

// ---- What an element is given by being the element it is ----

// What an element's size is of the size outside it.
enum class Step : std::uint8_t { kSame, kSmaller, kLarger, kTimes };

constexpr std::uint16_t kMono = 1U << 0U;          // Its text is in a face whose letters are of one width.
constexpr std::uint16_t kPre = 1U << 1U;           // Its white space is kept as written.
constexpr std::uint16_t kCenter = 1U << 2U;        // Its lines are centred.
constexpr std::uint16_t kKeepWithNext = 1U << 3U;  // A heading.
constexpr std::uint16_t kBullets = 1U << 4U;       // A list whose items have bullets.
constexpr std::uint16_t kNumbers = 1U << 5U;       // A list whose items are numbered.
constexpr std::uint16_t kPage = 1U << 6U;          // `html` or `body`: its margins are the page's.
constexpr std::uint16_t kNoAlign = 1U << 7U;       // Its `align` is not of its lines.
constexpr std::uint16_t kTerms = 1U << 8U;         // A list of terms and what they mean.
constexpr std::uint16_t kNotShown = 1U << 9U;      // What a page of text cannot show: nothing, and counted.
constexpr auto kItems = static_cast<std::uint16_t>(kBullets | kNumbers);  // A list of items.
constexpr auto kAnyList = static_cast<std::uint16_t>(kItems | kTerms);    // A list of either kind.

// The lines an element draws under or through its text, every bit but
// those, and the bits of text that is not on its line.
constexpr auto kLines = static_cast<std::uint16_t>(kTextUnderline | kTextLineThrough);
constexpr auto kNotLines = static_cast<std::uint16_t>(~kLines);
constexpr auto kMoved = static_cast<std::uint16_t>(kTextRaised | kTextLowered);

struct Kind {
  std::string_view name;
  StyleDisplay display = StyleDisplay::kInline;
  std::uint16_t bits = 0;  // kTextBold and the rest.
  Step step = Step::kSame;
  std::uint16_t times = 1000;    // For kTimes: thousandths of the size outside.
  std::uint16_t marginEm = 0;    // Above and below, thousandths of an em.
  std::uint8_t sidePx = 0;       // To the left and to the right.
  std::uint8_t leftPx = 0;       // To the left alone.
  std::uint8_t paddingLeft = 0;  // Padding to the left, in pixels.
  std::uint16_t flags = 0;
};

constexpr StyleDisplay kBlock = StyleDisplay::kBlock;
constexpr StyleDisplay kNone = StyleDisplay::kNone;

constexpr Kind Heading(std::string_view name, std::uint16_t times, std::uint16_t marginEm) noexcept {
  return {.name = name,
          .display = kBlock,
          .bits = kTextBold,
          .step = Step::kTimes,
          .times = times,
          .marginEm = marginEm,
          .flags = kKeepWithNext};
}

constexpr Kind List(std::string_view name, std::uint16_t which) noexcept {
  return {.name = name, .display = kBlock, .marginEm = 1000, .paddingLeft = 40, .flags = which};
}

constexpr Kind Verbatim(std::string_view name) noexcept {
  return {.name = name, .display = kBlock, .marginEm = 1000, .flags = static_cast<std::uint16_t>(kMono | kPre)};
}

// HTML's elements, as its own account of how they are shown has them
// (the HTML Standard's section on rendering; THIRD_PARTY.md), as far as a
// bundle keeps any of it; in the order of their names. An element that
// is not here is part of a line and is given nothing. Mathematics and
// the notes of a ruby are nothing here, which the standard shows, since
// a page of text cannot; and so are four elements that hold no text
// (`col`, `colgroup`, `source`, `track`).
constexpr auto kKinds = std::to_array<Kind>({
    {.name = "address", .display = kBlock, .bits = kTextItalic},
    {.name = "area", .display = kNone},
    {.name = "article", .display = kBlock},
    {.name = "aside", .display = kBlock},
    {.name = "b", .bits = kTextBold},
    {.name = "base", .display = kNone},
    {.name = "basefont", .display = kNone},
    {.name = "big", .step = Step::kLarger},
    {.name = "blockquote", .display = kBlock, .marginEm = 1000, .sidePx = 40},
    {.name = "body", .display = kBlock, .flags = kPage},
    {.name = "caption", .display = kBlock, .flags = static_cast<std::uint16_t>(kCenter | kNoAlign)},
    {.name = "center", .display = kBlock, .flags = kCenter},
    {.name = "cite", .bits = kTextItalic},
    {.name = "code", .flags = kMono},
    {.name = "col", .display = kNone},
    {.name = "colgroup", .display = kNone},
    {.name = "datalist", .display = kNone},
    {.name = "dd", .display = kBlock, .leftPx = 40},
    {.name = "del", .bits = kTextLineThrough},
    {.name = "details", .display = kBlock},
    {.name = "dfn", .bits = kTextItalic},
    List("dir", kBullets),
    {.name = "div", .display = kBlock},
    {.name = "dl", .display = kBlock, .marginEm = 1000, .flags = kTerms},
    {.name = "dt", .display = kBlock},
    {.name = "em", .bits = kTextItalic},
    {.name = "fieldset", .display = kBlock},
    {.name = "figcaption", .display = kBlock},
    {.name = "figure", .display = kBlock, .marginEm = 1000, .sidePx = 40},
    {.name = "footer", .display = kBlock},
    {.name = "form", .display = kBlock},
    Heading("h1", 2000, 670),
    Heading("h2", 1500, 830),
    Heading("h3", 1170, 1000),
    Heading("h4", 1000, 1330),
    Heading("h5", 830, 1670),
    Heading("h6", 670, 2330),
    {.name = "head", .display = kNone},
    {.name = "header", .display = kBlock},
    {.name = "hgroup", .display = kBlock},
    {.name = "hr", .display = kBlock, .marginEm = 500, .flags = kNoAlign},
    {.name = "html", .display = kBlock, .flags = kPage},
    {.name = "i", .bits = kTextItalic},
    {.name = "ins", .bits = kTextUnderline},
    {.name = "kbd", .flags = kMono},
    {.name = "legend", .display = kBlock},
    {.name = "li", .display = StyleDisplay::kListItem},
    {.name = "link", .display = kNone},
    Verbatim("listing"),
    {.name = "main", .display = kBlock},
    {.name = "math", .display = kNone, .flags = kNotShown},
    List("menu", kBullets),
    {.name = "meta", .display = kNone},
    {.name = "nav", .display = kBlock},
    {.name = "noembed", .display = kNone},
    {.name = "noframes", .display = kNone},
    List("ol", kNumbers),
    {.name = "p", .display = kBlock, .marginEm = 1000},
    {.name = "param", .display = kNone},
    Verbatim("plaintext"),
    Verbatim("pre"),
    {.name = "rp", .display = kNone},
    {.name = "rt", .display = kNone, .flags = kNotShown},
    {.name = "s", .bits = kTextLineThrough},
    {.name = "samp", .flags = kMono},
    {.name = "script", .display = kNone},
    {.name = "section", .display = kBlock},
    {.name = "small", .step = Step::kSmaller},
    {.name = "source", .display = kNone},
    {.name = "strike", .bits = kTextLineThrough},
    {.name = "strong", .bits = kTextBold},
    {.name = "style", .display = kNone},
    {.name = "sub", .bits = kTextLowered, .step = Step::kSmaller},
    {.name = "summary", .display = kBlock},
    {.name = "sup", .bits = kTextRaised, .step = Step::kSmaller},
    {.name = "table", .display = kBlock, .flags = kNoAlign},
    {.name = "tbody", .display = kBlock},
    {.name = "td", .display = kBlock},
    {.name = "template", .display = kNone},
    {.name = "tfoot", .display = kBlock},
    {.name = "th", .display = kBlock, .bits = kTextBold, .flags = kCenter},
    {.name = "thead", .display = kBlock},
    {.name = "title", .display = kNone},
    {.name = "tr", .display = kBlock},
    {.name = "track", .display = kNone},
    {.name = "tt", .flags = kMono},
    {.name = "u", .bits = kTextUnderline},
    List("ul", kBullets),
    {.name = "var", .bits = kTextItalic},
    Verbatim("xmp"),
});

constexpr bool InOrder() noexcept {
  for (std::size_t i = 1; i < kKinds.size(); ++i) {
    if (!(kKinds[i - 1].name < kKinds[i].name)) {
      return false;
    }
  }
  return true;
}
static_assert(InOrder(), "the elements are each once, in the order of their names");

// The longest name there, and so the most of a name that need be looked at.
constexpr std::size_t LongestName() noexcept {
  std::size_t longest = 0;
  for (const Kind& kind : kKinds) {
    longest = std::max(longest, kind.name.size());
  }
  return longest;
}
constexpr std::size_t kLongestName = LongestName();

char Small(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

bool SameButForCase(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Small(x) == y; });
}

// What the element of this name is given, in whatever case it is written.
const Kind& KindOf(std::string_view name) noexcept {
  static constexpr Kind kPlain{};
  if (name.empty() || name.size() > kLongestName) {
    return kPlain;
  }
  std::array<char, kLongestName> small{};
  std::transform(name.begin(), name.end(), small.begin(), Small);
  const std::string_view wanted(small.data(), name.size());
  const auto* found = std::lower_bound(kKinds.begin(), kKinds.end(), wanted,
                                       [](const Kind& kind, std::string_view want) { return kind.name < want; });
  return found != kKinds.end() && found->name == wanted ? *found : kPlain;
}

// ---- Amounts ----

// `a` over `b`, to the nearest whole number, a half going away from
// nothing. `b` is above nothing.
constexpr std::int64_t Over(std::int64_t a, std::int64_t b) noexcept {
  return a >= 0 ? (a + (b / 2)) / b : -((-a + (b / 2)) / b);
}

constexpr std::uint16_t AsSize(std::int64_t size) noexcept {
  return static_cast<std::uint16_t>(std::clamp<std::int64_t>(size, 0, 65535));
}

constexpr std::int16_t AsPart(std::int64_t part) noexcept {
  return static_cast<std::int16_t>(std::clamp<std::int64_t>(part, -32768, 32767));
}

// The sizes CSS has names for, in thousandths of the reader's.
constexpr std::uint16_t NamedSize(CssKeyword name) noexcept {
  switch (name) {
    case CssKeyword::kXxSmall:
      return 563;  // 9 sixteenths.
    case CssKeyword::kXSmall:
      return 625;
    case CssKeyword::kSmall:
      return 813;  // 13 sixteenths.
    case CssKeyword::kLarge:
      return 1125;
    case CssKeyword::kXLarge:
      return 1500;
    case CssKeyword::kXxLarge:
      return 2000;
    default:
      return 1000;
  }
}

constexpr std::uint16_t Smaller(std::uint16_t size) noexcept { return AsSize(Over(std::int64_t{size} * 5, 6)); }
constexpr std::uint16_t Larger(std::uint16_t size) noexcept { return AsSize(Over(std::int64_t{size} * 6, 5)); }

// A length a sheet wrote, as the three amounts: an em is of `size`, a rem
// of `root`, both thousandths of the reader's.
StyleLength AsLength(const CssValue& value, std::uint16_t size, std::uint16_t root) noexcept {
  StyleLength length;
  const std::int64_t number = value.number;
  switch (value.unit) {
    case CssUnit::kPx:
    case CssUnit::kNumber:  // Only ever nothing, which is nothing of anything.
      length.px = AsPart(Over(number * 8, 1000));
      break;
    case CssUnit::kEm:
      length.em = AsPart(Over(number * size * 64, 1'000'000));
      break;
    case CssUnit::kRem:
      length.em = AsPart(Over(number * root * 64, 1'000'000));
      break;
    case CssUnit::kPercent:
      length.percent = AsPart(Over(number * 64, 1000));
      break;
  }
  return length;
}

constexpr StyleLength Pixels(std::int64_t pixels) noexcept { return {.em = 0, .px = AsPart(pixels * 8), .percent = 0}; }

// So many thousandths of an em of a text of this size.
constexpr StyleLength Ems(std::int64_t thousandths, std::uint16_t size) noexcept {
  return {.em = AsPart(Over(thousandths * size * 64, 1'000'000)), .px = 0, .percent = 0};
}

// ---- What a sheet said, as what it means ----

bool Is(const CssStyle& style, CssProperty property, CssKeyword keyword) noexcept {
  return style.Has(property) && style[property].kind == CssValue::Kind::kKeyword && style[property].keyword == keyword;
}

bool IsLength(const CssStyle& style, CssProperty property) noexcept {
  return style.Has(property) && style[property].kind == CssValue::Kind::kLength;
}

StyleAlign AsAlign(CssKeyword keyword) noexcept {
  switch (keyword) {
    case CssKeyword::kLeft:
      return StyleAlign::kLeft;
    case CssKeyword::kRight:
      return StyleAlign::kRight;
    case CssKeyword::kCenter:
      return StyleAlign::kCenter;
    case CssKeyword::kJustify:
      return StyleAlign::kJustify;
    case CssKeyword::kStart:
      return StyleAlign::kStart;
    case CssKeyword::kEnd:
      return StyleAlign::kEnd;
    default:
      return StyleAlign::kUnset;
  }
}

StyleMarker AsMarker(CssKeyword keyword) noexcept {
  switch (keyword) {
    case CssKeyword::kNone:
      return StyleMarker::kNone;
    case CssKeyword::kCircle:
      return StyleMarker::kCircle;
    case CssKeyword::kSquare:
      return StyleMarker::kSquare;
    case CssKeyword::kDecimal:
      return StyleMarker::kDecimal;
    case CssKeyword::kDecimalLeadingZero:
      return StyleMarker::kDecimalLeadingZero;
    case CssKeyword::kLowerAlpha:
      return StyleMarker::kLowerAlpha;
    case CssKeyword::kUpperAlpha:
      return StyleMarker::kUpperAlpha;
    case CssKeyword::kLowerRoman:
      return StyleMarker::kLowerRoman;
    case CssKeyword::kUpperRoman:
      return StyleMarker::kUpperRoman;
    default:
      return StyleMarker::kDisc;
  }
}

// What `align` may say of a block's lines, and `type` of what stands
// before a list's items: the letters of a numbered list go by their case,
// a bullet's name is in any.
struct AlignWord {
  std::string_view word;
  StyleAlign align;
};
constexpr std::array<AlignWord, 4> kAlignWords = {{{"left", StyleAlign::kLeft},
                                                   {"right", StyleAlign::kRight},
                                                   {"center", StyleAlign::kCenter},
                                                   {"justify", StyleAlign::kJustify}}};
struct MarkerWord {
  std::string_view word;
  StyleMarker marker;
};
constexpr std::array<MarkerWord, 5> kNumberTypes = {{{"1", StyleMarker::kDecimal},
                                                     {"a", StyleMarker::kLowerAlpha},
                                                     {"A", StyleMarker::kUpperAlpha},
                                                     {"i", StyleMarker::kLowerRoman},
                                                     {"I", StyleMarker::kUpperRoman}}};
constexpr std::array<MarkerWord, 3> kBulletTypes = {
    {{"disc", StyleMarker::kDisc}, {"circle", StyleMarker::kCircle}, {"square", StyleMarker::kSquare}}};

// The attributes that say something of how an element is shown.
constexpr std::size_t kAskHidden = 0;
constexpr std::size_t kAskXmlLang = 1;
constexpr std::size_t kAskLang = 2;
constexpr std::size_t kAskAlign = 3;
constexpr std::size_t kAskType = 4;
constexpr std::size_t kAskDir = 5;
constexpr std::array<std::string_view, 6> kAskedNames = {"hidden", "xml:lang", "lang", "align", "type", "dir"};

// Those of them an element has, each with its value.
struct Asked {
  std::array<std::string_view, kAskedNames.size()> values;
  std::uint8_t has = 0;

  bool Has(std::size_t which) const noexcept { return ((std::uint32_t{has} >> which) & 1U) != 0; }
};

// Goes once through an element's attributes.
Asked AskedOf(const XmlAttributes& attributes) noexcept {
  Asked asked;
  for (std::size_t i = 0; i < attributes.Count(); ++i) {
    const std::string_view name = attributes.Name(i);
    for (std::size_t which = 0; which < kAskedNames.size(); ++which) {
      if (name == kAskedNames[which] && !asked.Has(which)) {
        asked.values[which] = attributes.Value(i);
        asked.has = static_cast<std::uint8_t>(asked.has | (1U << which));
      }
    }
  }
  return asked;
}

// One bit of how text is set, and who said so.
void SetBit(TextStyle& text, std::uint16_t bit, bool on, bool book) noexcept {
  const auto others = static_cast<std::uint16_t>(~bit);
  text.bits = static_cast<std::uint16_t>(on ? text.bits | bit : text.bits & others);
  text.fromBook = static_cast<std::uint16_t>(book ? text.fromBook | bit : text.fromBook & others);
}

// What a sheet set of how text is set, put on `text`, which begins as
// what the text outside has: for an element, or for its first letter.
// `outside` is what `inherit` takes; `inLine` is whether the text can be
// raised or lowered by it; `root` is what a rem is of.
void SetText(const CssStyle& style, const TextStyle& outside, bool inLine, std::uint16_t root,
             TextStyle& text) noexcept {
  if (style.Has(CssProperty::kFontSize)) {
    const CssValue& value = style[CssProperty::kFontSize];
    const std::int64_t base = outside.size;
    if (value.kind == CssValue::Kind::kLength) {
      const std::int64_t number = value.number;
      switch (value.unit) {
        case CssUnit::kEm:
          text.size = AsSize(Over(base * number, 1000));
          break;
        case CssUnit::kPercent:
          text.size = AsSize(Over(base * number, 100'000));
          break;
        case CssUnit::kRem:
          text.size = AsSize(Over(std::int64_t{root} * number, 1000));
          break;
        case CssUnit::kPx:
        case CssUnit::kNumber:
          text.size = AsSize(Over(number, 16));
          break;
      }
    } else if (value.keyword == CssKeyword::kSmaller) {
      text.size = Smaller(outside.size);
    } else if (value.keyword == CssKeyword::kLarger) {
      text.size = Larger(outside.size);
    } else if (value.keyword == CssKeyword::kInherit) {
      text.size = outside.size;
    } else {
      text.size = NamedSize(value.keyword);  // And `initial`, which is `medium`.
    }
    text.sizeFromBook = true;
  }
  if (style.Has(CssProperty::kFontWeight)) {
    const CssValue& value = style[CssProperty::kFontWeight];
    const bool bold = value.kind == CssValue::Kind::kLength ? value.number >= 600'000
                      : value.keyword == CssKeyword::kInherit
                          ? (outside.bits & kTextBold) != 0
                          : value.keyword == CssKeyword::kBold || value.keyword == CssKeyword::kBolder;
    SetBit(text, kTextBold, bold, true);
  }
  if (style.Has(CssProperty::kFontStyle)) {
    const CssKeyword keyword = style[CssProperty::kFontStyle].keyword;
    const bool italic = keyword == CssKeyword::kInherit
                            ? (outside.bits & kTextItalic) != 0
                            : keyword == CssKeyword::kItalic || keyword == CssKeyword::kOblique;
    SetBit(text, kTextItalic, italic, true);
  }
  if (style.Has(CssProperty::kFontVariant)) {
    const CssKeyword keyword = style[CssProperty::kFontVariant].keyword;
    const bool small =
        keyword == CssKeyword::kInherit ? (outside.bits & kTextSmallCaps) != 0 : keyword == CssKeyword::kSmallCaps;
    SetBit(text, kTextSmallCaps, small, true);
  }
  // A line under or through adds to what there is: nothing takes away
  // the one an element outside drew.
  if (Is(style, CssProperty::kTextDecoration, CssKeyword::kUnderline)) {
    SetBit(text, kTextUnderline, true, true);
  } else if (Is(style, CssProperty::kTextDecoration, CssKeyword::kLineThrough)) {
    SetBit(text, kTextLineThrough, true, true);
  }
  if (inLine && style.Has(CssProperty::kVerticalAlign)) {
    const CssValue& value = style[CssProperty::kVerticalAlign];
    bool raised = false;
    bool lowered = false;
    if (value.kind == CssValue::Kind::kLength) {
      raised = value.number > 0;
      lowered = value.number < 0;
    } else if (value.keyword == CssKeyword::kInherit) {
      raised = (outside.bits & kTextRaised) != 0;
      lowered = (outside.bits & kTextLowered) != 0;
    } else {
      raised = value.keyword == CssKeyword::kSuper || value.keyword == CssKeyword::kTop ||
               value.keyword == CssKeyword::kTextTop;
      lowered = value.keyword == CssKeyword::kSub || value.keyword == CssKeyword::kBottom ||
                value.keyword == CssKeyword::kTextBottom;
    }
    SetBit(text, kTextRaised, raised, true);
    SetBit(text, kTextLowered, lowered, true);
  }
  if (style.Has(CssProperty::kFontFamily)) {
    const CssValue& value = style[CssProperty::kFontFamily];
    if (value.kind == CssValue::Kind::kFamilies) {
      text.family = value.familyClass;
    } else {
      text.family = value.keyword == CssKeyword::kInherit ? outside.family : CssFamilyClass::kUnset;
    }
    text.familyFromBook = true;
  }
}

constexpr std::array<CssProperty, 4> kMargins = {CssProperty::kMarginTop, CssProperty::kMarginRight,
                                                 CssProperty::kMarginBottom, CssProperty::kMarginLeft};
constexpr std::array<CssProperty, 4> kPaddings = {CssProperty::kPaddingTop, CssProperty::kPaddingRight,
                                                  CssProperty::kPaddingBottom, CssProperty::kPaddingLeft};

// The flags of a level.
constexpr std::uint16_t kHidden = 1U << 0U;  // It is nothing, and so is all inside it.
constexpr std::uint16_t kFamilyFromBook = 1U << 1U;
constexpr std::uint16_t kSizeFromBook = 1U << 2U;
constexpr std::uint16_t kAlignFromBook = 1U << 3U;
constexpr std::uint16_t kHasIndent = 1U << 4U;
constexpr std::uint16_t kIndentFromBook = 1U << 5U;
constexpr std::uint16_t kLineFromBook = 1U << 6U;
constexpr std::uint16_t kMarkerFromBook = 1U << 7U;
constexpr std::uint16_t kRightToLeft = 1U << 8U;
constexpr std::uint16_t kSpacesAsWritten = 1U << 9U;
constexpr std::uint16_t kInList = 1U << 10U;  // It is a list, a list of terms among them, or inside one.

}  // namespace

// What is kept of an open element: what the elements inside it take from
// it.
struct StyleStack::Level {
  std::uint16_t size = 1000;
  std::uint16_t bits = 0;
  std::uint16_t fromBook = 0;
  std::uint16_t lineMultiple = 0;
  StyleLength lineLength;
  StyleLength indent;
  std::uint8_t family = 0;
  std::uint8_t language = 0;
  std::uint8_t align = 0;
  std::uint8_t marker = static_cast<std::uint8_t>(StyleMarker::kDisc);
  std::uint8_t lineKind = 0;
  std::uint8_t lists = 0;  // How many lists it is inside, itself among them; held to 255.
  std::uint16_t flags = 0;
};

const char* StyleLeftOutName(StyleLeftOut kind) noexcept {
  switch (kind) {
    case StyleLeftOut::kTooDeep:
      return "an element too deep";
    case StyleLeftOut::kPageMargin:
      return "a page margin";
    case StyleLeftOut::kInheritOfOwn:
      return "inherit for what is an element's own";
    case StyleLeftOut::kLanguage:
      return "a language with no room";
    case StyleLeftOut::kBreakAvoidBefore:
      return "no page to end before";
    case StyleLeftOut::kNotShown:
      return "what cannot be shown";
  }
  return "?";
}

const char* StyleDisplayName(StyleDisplay display) noexcept {
  switch (display) {
    case StyleDisplay::kNone:
      return "none";
    case StyleDisplay::kInline:
      return "inline";
    case StyleDisplay::kBlock:
      return "block";
    case StyleDisplay::kListItem:
      return "list-item";
  }
  return "?";
}

const char* StyleAlignName(StyleAlign align) noexcept {
  switch (align) {
    case StyleAlign::kUnset:
      return "unset";
    case StyleAlign::kLeft:
      return "left";
    case StyleAlign::kRight:
      return "right";
    case StyleAlign::kCenter:
      return "center";
    case StyleAlign::kJustify:
      return "justify";
    case StyleAlign::kStart:
      return "start";
    case StyleAlign::kEnd:
      return "end";
  }
  return "?";
}

const char* StyleMarkerName(StyleMarker marker) noexcept {
  switch (marker) {
    case StyleMarker::kNone:
      return "none";
    case StyleMarker::kDisc:
      return "disc";
    case StyleMarker::kCircle:
      return "circle";
    case StyleMarker::kSquare:
      return "square";
    case StyleMarker::kDecimal:
      return "decimal";
    case StyleMarker::kDecimalLeadingZero:
      return "decimal-leading-zero";
    case StyleMarker::kLowerAlpha:
      return "lower-alpha";
    case StyleMarker::kUpperAlpha:
      return "upper-alpha";
    case StyleMarker::kLowerRoman:
      return "lower-roman";
    case StyleMarker::kUpperRoman:
      return "upper-roman";
  }
  return "?";
}

StyleStack::StyleStack(std::span<std::uint8_t> memory) noexcept {
  static_assert(sizeof(Level) == kLevelBytes, "a level is as large as the header says");
  if (memory.size() >= kLanguagesBytes + kLevelBytes) {
    tags_ = memory.data();
    memory_ = memory.subspan(kLanguagesBytes).data();
    levels_ = (memory.size() - kLanguagesBytes) / kLevelBytes;
  }
}

void StyleStack::Begin() noexcept {
  // The size of the document's own element is set when one opens.
  depth_ = 0;
  languages_ = 0;
  hiddenFrom_ = kNotHidden;
  leftOut_ = {};
}

StyleStack::Level StyleStack::Load(std::size_t index) const noexcept {
  Level level;
  std::memcpy(&level, memory_ + (index * kLevelBytes), kLevelBytes);
  return level;
}

void StyleStack::Store(std::size_t index, const Level& level) noexcept {
  std::memcpy(memory_ + (index * kLevelBytes), &level, kLevelBytes);
}

StyleStack::Level StyleStack::Outside() const noexcept {
  const std::size_t kept = std::min(depth_, levels_);
  return kept == 0 ? Level{} : Load(kept - 1);
}

std::string_view StyleStack::Language(std::uint8_t language) const noexcept {
  if (language == 0 || language > languages_) {
    return {};
  }
  const std::uint8_t* tag = tags_ + ((language - 1U) * (kMaxLanguageBytes + 1));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  return {reinterpret_cast<const char*>(tag + 1), tag[0]};
}

std::uint8_t StyleStack::LanguageOf(std::string_view tag, std::uint8_t outside) noexcept {
  for (std::size_t i = 1; i <= languages_; ++i) {
    ++work_;
    const std::string_view known = Language(static_cast<std::uint8_t>(i));
    if (known.size() == tag.size() &&
        std::equal(known.begin(), known.end(), tag.begin(), [](char a, char b) { return Small(a) == Small(b); })) {
      return static_cast<std::uint8_t>(i);
    }
  }
  if (tags_ == nullptr || languages_ == kMaxLanguages || tag.size() > kMaxLanguageBytes) {
    Count(StyleLeftOut::kLanguage);
    return outside;
  }
  std::uint8_t* kept = tags_ + (languages_ * (kMaxLanguageBytes + 1));
  kept[0] = static_cast<std::uint8_t>(tag.size());
  std::memcpy(kept + 1, tag.data(), tag.size());
  return static_cast<std::uint8_t>(++languages_);
}

void StyleStack::Open(std::string_view name, const XmlAttributes& attributes, const CssStyle& style,
                      const CssStyle& firstLetter, ElementStyle& said) noexcept {
  said = {};
  work_ = 0;
  const Level outside = Outside();
  const bool kept = depth_ < levels_;
  if (!kept) {
    Count(StyleLeftOut::kTooDeep);
  }
  const Kind& kind = KindOf(name);

  // What it is to the lines of a page.
  StyleDisplay display = kind.display;
  if (style.Has(CssProperty::kDisplay)) {
    switch (style[CssProperty::kDisplay].keyword) {
      case CssKeyword::kBlock:
        display = StyleDisplay::kBlock;
        break;
      case CssKeyword::kInline:
      case CssKeyword::kInitial:  // The property's own start.
        display = StyleDisplay::kInline;
        break;
      case CssKeyword::kListItem:
        display = StyleDisplay::kListItem;
        break;
      case CssKeyword::kNone:
        display = StyleDisplay::kNone;
        break;
      case CssKeyword::kInherit:
        Count(StyleLeftOut::kInheritOfOwn);
        break;
      default:
        break;  // `inline-block`: as the element is.
    }
  }
  // What its attributes say is asked only of an element that may be
  // shown.
  const bool insideNothing = (outside.flags & kHidden) != 0 || hiddenFrom_ != kNotHidden;
  Asked asked;
  if (!insideNothing) {
    asked = AskedOf(attributes);
    work_ += static_cast<std::uint32_t>(attributes.Count());
  }
  if (insideNothing || asked.Has(kAskHidden)) {
    display = StyleDisplay::kNone;
  }
  if (display == StyleDisplay::kNone) {
    if (!insideNothing && (kind.flags & kNotShown) != 0) {
      Count(StyleLeftOut::kNotShown);
    }
    said.display = StyleDisplay::kNone;
    if (kept) {
      Level level = outside;
      level.flags = static_cast<std::uint16_t>(level.flags | kHidden);
      Store(depth_, level);
    } else if (hiddenFrom_ == kNotHidden) {
      hiddenFrom_ = depth_;
    }
    ++depth_;
    return;
  }
  said.display = display;
  const bool inLine = display == StyleDisplay::kInline;

  // How its text is set: as the text outside, then as the element has
  // it, then as the book has it.
  TextStyle before;
  before.bits = outside.bits;
  before.fromBook = outside.fromBook;
  before.family = static_cast<CssFamilyClass>(outside.family);
  before.familyFromBook = (outside.flags & kFamilyFromBook) != 0;
  before.language = outside.language;
  before.size = outside.size;
  before.sizeFromBook = (outside.flags & kSizeFromBook) != 0;
  TextStyle text = before;
  if (!inLine) {
    // What is raised or lowered is so within a line, and no further.
    SetBit(text, kTextRaised, false, false);
    SetBit(text, kTextLowered, false, false);
  }
  switch (kind.step) {
    case Step::kSame:
      break;
    case Step::kSmaller:
      text.size = Smaller(outside.size);
      break;
    case Step::kLarger:
      text.size = Larger(outside.size);
      break;
    case Step::kTimes:
      text.size = AsSize(Over(std::int64_t{outside.size} * kind.times, 1000));
      break;
  }
  // What the element draws under or through its text gives way to what
  // the book says of the element, whatever that is.
  const std::uint16_t given =
      style.Has(CssProperty::kTextDecoration) ? static_cast<std::uint16_t>(kind.bits & kNotLines) : kind.bits;
  for (const std::uint16_t bit : {kTextBold, kTextItalic, kTextUnderline, kTextLineThrough}) {
    if ((given & bit) != 0 && (text.bits & bit) == 0) {
      SetBit(text, bit, true, false);
    }
  }
  if (inLine && (kind.bits & kMoved) != 0) {
    SetBit(text, kTextRaised, (kind.bits & kTextRaised) != 0, false);
    SetBit(text, kTextLowered, (kind.bits & kTextLowered) != 0, false);
  }
  if ((kind.flags & kMono) != 0) {
    text.family = CssFamilyClass::kMono;
    text.familyFromBook = false;
  }
  SetText(style, before, inLine, depth_ == 0 ? std::uint16_t{1000} : rootSize_, text);
  if (asked.Has(kAskXmlLang) || asked.Has(kAskLang)) {
    const std::string_view tag = asked.values[asked.Has(kAskXmlLang) ? kAskXmlLang : kAskLang];
    text.language = tag.empty() ? std::uint8_t{0} : LanguageOf(tag, outside.language);
  }
  if (depth_ == 0) {
    rootSize_ = text.size;
  }
  said.text = text;

  // What is set of a block's lines.
  said.align = static_cast<StyleAlign>(outside.align);
  said.alignFromBook = (outside.flags & kAlignFromBook) != 0;
  if ((kind.flags & kCenter) != 0) {
    said.align = StyleAlign::kCenter;
    said.alignFromBook = false;
  }
  if (!inLine && (kind.flags & kNoAlign) == 0 && asked.Has(kAskAlign)) {
    for (const AlignWord& known : kAlignWords) {
      if (SameButForCase(asked.values[kAskAlign], known.word)) {
        said.align = known.align;
        said.alignFromBook = true;
      }
    }
  }
  if (style.Has(CssProperty::kTextAlign)) {
    const CssKeyword keyword = style[CssProperty::kTextAlign].keyword;
    said.align = keyword == CssKeyword::kInherit ? static_cast<StyleAlign>(outside.align) : AsAlign(keyword);
    said.alignFromBook = true;
  }

  said.hasIndent = (outside.flags & kHasIndent) != 0;
  said.indent = outside.indent;
  said.indentFromBook = (outside.flags & kIndentFromBook) != 0;
  if (style.Has(CssProperty::kTextIndent)) {
    const CssValue& value = style[CssProperty::kTextIndent];
    if (value.kind == CssValue::Kind::kLength) {
      said.indent = AsLength(value, text.size, rootSize_);
    } else if (value.keyword != CssKeyword::kInherit) {
      said.indent = {};
    }
    said.hasIndent = true;
    said.indentFromBook = true;
  }

  said.lineHeight.kind = static_cast<StyleLineHeight::Kind>(outside.lineKind);
  said.lineHeight.multiple = outside.lineMultiple;
  said.lineHeight.length = outside.lineLength;
  said.lineHeightFromBook = (outside.flags & kLineFromBook) != 0;
  if (style.Has(CssProperty::kLineHeight)) {
    const CssValue& value = style[CssProperty::kLineHeight];
    if (value.kind == CssValue::Kind::kLength) {
      const std::int64_t number = value.number;
      said.lineHeight = {};
      if (value.unit == CssUnit::kNumber || value.unit == CssUnit::kEm) {
        said.lineHeight.kind = StyleLineHeight::Kind::kMultiple;
        said.lineHeight.multiple = AsSize(number);
      } else if (value.unit == CssUnit::kPercent) {
        said.lineHeight.kind = StyleLineHeight::Kind::kMultiple;
        said.lineHeight.multiple = AsSize(Over(number, 100));
      } else {
        said.lineHeight.kind = StyleLineHeight::Kind::kLength;
        said.lineHeight.length = AsLength(value, text.size, rootSize_);
      }
    } else if (value.keyword != CssKeyword::kInherit) {
      said.lineHeight = {};  // `normal`, and `initial`.
    }
    said.lineHeightFromBook = true;
  }

  // What stands before the items of a list: by the list, and by how deep
  // among lists it is; then as `type` says; then as the book says.
  std::uint8_t lists = outside.lists;
  said.marker = static_cast<StyleMarker>(outside.marker);
  said.markerFromBook = (outside.flags & kMarkerFromBook) != 0;
  if ((kind.flags & kItems) != 0) {
    lists = static_cast<std::uint8_t>(std::min<unsigned>(lists + 1U, 255U));
    said.marker = (kind.flags & kNumbers) != 0 ? StyleMarker::kDecimal
                  : lists == 1                 ? StyleMarker::kDisc
                  : lists == 2                 ? StyleMarker::kCircle
                                               : StyleMarker::kSquare;
    said.markerFromBook = false;
  }
  if (((kind.flags & kItems) != 0 || display == StyleDisplay::kListItem) && asked.Has(kAskType)) {
    const std::string_view type = asked.values[kAskType];
    for (const MarkerWord& known : kNumberTypes) {
      if (type == known.word) {
        said.marker = known.marker;
        said.markerFromBook = true;
      }
    }
    for (const MarkerWord& known : kBulletTypes) {
      if (SameButForCase(type, known.word)) {
        said.marker = known.marker;
        said.markerFromBook = true;
      }
    }
  }
  if (style.Has(CssProperty::kListStyleType)) {
    const CssKeyword keyword = style[CssProperty::kListStyleType].keyword;
    said.marker = keyword == CssKeyword::kInherit ? static_cast<StyleMarker>(outside.marker) : AsMarker(keyword);
    said.markerFromBook = true;
  }

  said.rightToLeft = (outside.flags & kRightToLeft) != 0;
  if (asked.Has(kAskDir)) {
    if (SameButForCase(asked.values[kAskDir], "rtl")) {
      said.rightToLeft = true;
    } else if (SameButForCase(asked.values[kAskDir], "ltr")) {
      said.rightToLeft = false;
    }
  }
  said.spacesAsWritten = (outside.flags & kSpacesAsWritten) != 0 || (kind.flags & kPre) != 0;

  // What is its own: the space around it.
  const bool page = (kind.flags & kPage) != 0;
  // A list inside a list, lists of terms among them, has no space above
  // or below.
  const bool inList = (outside.flags & kInList) != 0;
  const bool list = (kind.flags & kAnyList) != 0;
  const std::int64_t marginEm = list && inList ? 0 : kind.marginEm;
  said.margin[kStyleTop] = Ems(marginEm, text.size);
  said.margin[kStyleBottom] = Ems(marginEm, text.size);
  said.margin[kStyleLeft] = Pixels(std::int64_t{kind.sidePx} + kind.leftPx);
  said.margin[kStyleRight] = Pixels(kind.sidePx);
  said.padding[kStyleLeft] = Pixels(kind.paddingLeft);
  for (std::size_t side = 0; side < 4; ++side) {
    for (const bool margin : {true, false}) {
      const CssProperty property = margin ? kMargins[side] : kPaddings[side];
      if (!style.Has(property)) {
        continue;
      }
      const CssValue& value = style[property];
      if (value.kind == CssValue::Kind::kKeyword && value.keyword == CssKeyword::kInherit) {
        Count(StyleLeftOut::kInheritOfOwn);
        continue;
      }
      // A length, or `auto` or `initial`, which are no space.
      const StyleLength length =
          value.kind == CssValue::Kind::kLength ? AsLength(value, text.size, rootSize_) : StyleLength{};
      if (page) {
        if (!length.IsZero()) {
          Count(StyleLeftOut::kPageMargin);
        }
        continue;
      }
      const auto bit = static_cast<std::uint8_t>(1U << side);
      if (margin) {
        said.margin[side] = length;
        said.marginFromBook = static_cast<std::uint8_t>(said.marginFromBook | bit);
        const bool isAuto = value.kind == CssValue::Kind::kKeyword && value.keyword == CssKeyword::kAuto;
        if (side == kStyleLeft) {
          said.marginLeftAuto = isAuto;
        } else if (side == kStyleRight) {
          said.marginRightAuto = isAuto;
        }
      } else {
        said.padding[side] = length;
        said.paddingFromBook = static_cast<std::uint8_t>(said.paddingFromBook | bit);
      }
    }
  }

  // Its width and height, the side it is floated to, and where a page
  // may end.
  for (const bool width : {true, false}) {
    const CssProperty property = width ? CssProperty::kWidth : CssProperty::kHeight;
    if (Is(style, property, CssKeyword::kInherit)) {
      Count(StyleLeftOut::kInheritOfOwn);
    } else if (IsLength(style, property)) {
      (width ? said.hasWidth : said.hasHeight) = true;
      (width ? said.width : said.height) = AsLength(style[property], text.size, rootSize_);
    }
  }
  if (Is(style, CssProperty::kFloat, CssKeyword::kLeft)) {
    said.floated = StyleFloat::kLeft;
  } else if (Is(style, CssProperty::kFloat, CssKeyword::kRight)) {
    said.floated = StyleFloat::kRight;
  } else if (Is(style, CssProperty::kFloat, CssKeyword::kInherit)) {
    Count(StyleLeftOut::kInheritOfOwn);
  }
  said.keepWithNext = (kind.flags & kKeepWithNext) != 0;
  if (style.Has(CssProperty::kBreakBefore)) {
    const CssKeyword keyword = style[CssProperty::kBreakBefore].keyword;
    said.breakBefore = keyword == CssKeyword::kAlways;
    if (keyword == CssKeyword::kAvoid) {
      Count(StyleLeftOut::kBreakAvoidBefore);
    } else if (keyword == CssKeyword::kInherit) {
      Count(StyleLeftOut::kInheritOfOwn);
    }
  }
  if (style.Has(CssProperty::kBreakAfter)) {
    const CssKeyword keyword = style[CssProperty::kBreakAfter].keyword;
    if (keyword == CssKeyword::kInherit) {
      Count(StyleLeftOut::kInheritOfOwn);
    } else {
      said.breakAfter = keyword == CssKeyword::kAlways;
      said.keepWithNext = keyword == CssKeyword::kAvoid;
      said.keepWithNextFromBook = true;
    }
  }
  if (style.Has(CssProperty::kBreakInside)) {
    const CssKeyword keyword = style[CssProperty::kBreakInside].keyword;
    said.keepTogether = keyword == CssKeyword::kAvoid;
    if (keyword == CssKeyword::kInherit) {
      Count(StyleLeftOut::kInheritOfOwn);
    }
  }

  // Its first letter: as its own text, then as the rule for the letter
  // has it.
  if (firstLetter.set != 0) {
    said.hasFirstLetter = true;
    said.firstLetter = text;
    SetText(firstLetter, text, true, rootSize_, said.firstLetter);
    said.firstLetterFloated = Is(firstLetter, CssProperty::kFloat, CssKeyword::kLeft) ||
                              Is(firstLetter, CssProperty::kFloat, CssKeyword::kRight);
  }

  if (kept) {
    Level level;
    level.size = text.size;
    level.bits = text.bits;
    level.fromBook = text.fromBook;
    level.lineMultiple = said.lineHeight.multiple;
    level.lineLength = said.lineHeight.length;
    level.indent = said.indent;
    level.family = static_cast<std::uint8_t>(text.family);
    level.language = text.language;
    level.align = static_cast<std::uint8_t>(said.align);
    level.marker = static_cast<std::uint8_t>(said.marker);
    level.lineKind = static_cast<std::uint8_t>(said.lineHeight.kind);
    level.lists = lists;
    level.flags = static_cast<std::uint16_t>(
        (text.familyFromBook ? kFamilyFromBook : 0U) | (text.sizeFromBook ? kSizeFromBook : 0U) |
        (said.alignFromBook ? kAlignFromBook : 0U) | (said.hasIndent ? kHasIndent : 0U) |
        (said.indentFromBook ? kIndentFromBook : 0U) | (said.lineHeightFromBook ? kLineFromBook : 0U) |
        (said.markerFromBook ? kMarkerFromBook : 0U) | (said.rightToLeft ? kRightToLeft : 0U) |
        (said.spacesAsWritten ? kSpacesAsWritten : 0U) | (list || inList ? kInList : 0U));
    Store(depth_, level);
  }
  ++depth_;
}

void StyleStack::Close() noexcept {
  if (depth_ != 0) {
    --depth_;
  }
  if (depth_ == hiddenFrom_) {
    hiddenFrom_ = kNotHidden;
  }
}

}  // namespace bn::epub
