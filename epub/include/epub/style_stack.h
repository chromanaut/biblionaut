#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "epub/css.h"
#include "epub/css_rules.h"
#include "epub/style_values.h"
#include "epub/xml.h"

// What each element of a document comes to.
//
// A cascade (css_rules.h) says what the book's rules set for an element,
// as the sheets wrote it: "1.5em", "larger", "inherit", and nothing at all
// for most properties of most elements. A StyleStack goes through the
// document beside it and works the rest out: what an element takes from
// the one it is inside, what it is given by being the element it is (an
// `h1` is large and bold, a `blockquote` stands in from both sides), and
// what the book's values come to as amounts: a size as so much of the
// reader's own, a length as so much of that size, of a pixel and of the
// page's width.
//
// It keeps a few bytes for each element that is open, in the memory it is
// given, and nothing else of the document. What it says of an element is
// said once, as the element opens.
//
// The rules it goes by:
//
//   - Whether an element is a block, part of a line or nothing is the
//     book's `display` where that is `block`, `inline`, `list-item` or
//     `none`, and otherwise what the element is, by HTML's own list.
//     `inline-block` changes nothing, and `initial` is part of a line,
//     as the property starts. An element with `hidden`, and
//     everything inside an element that is nothing, is nothing.
//     Mathematics (`math`) and the notes of a `ruby` (`rt`) are nothing
//     by that list, since a page of text cannot show them, and each is
//     counted; a ruby's own text is kept.
//   - A size is so many thousandths of the reader's text size. An em and
//     a percentage are of the size of the element outside; a pixel is a
//     sixteenth of the reader's size; `smaller` and `larger` are a step
//     of 1.2; the sizes by name are CSS's, with `medium` the reader's.
//   - Bold is a weight of 600 or over, or `bolder`. Underlining and
//     striking add up through the elements a text is inside. Text is
//     raised or lowered by `vertical-align` on something that is part of
//     a line: `super`, `top`, `text-top` or a length above nothing raise
//     it, `sub`, `bottom`, `text-bottom` or a length below nothing lower
//     it; and it stays so through what is inside, as far as the next
//     block.
//   - What an element is given by being what it is counts as the
//     element's; what a rule or a `style` sets counts as the book's, and
//     so does what `align`, and `type` on a list, say. Each thing said
//     comes with which of the two it is, so that a reader's own choices
//     can stand in for a book's without the book being read again.
//   - `inherit` is what the element outside has, for the properties that
//     pass from one element to those inside it. For a property that is an
//     element's own (its margins and padding, its width and height,
//     `display`, `float`, where a page breaks) it is taken as nothing
//     said, and counted. `initial` is the property's own start.
//   - The margins and the padding of `html` and `body` are left out, and
//     counted: they are the book's page margins, and a page's margins are
//     the reader's.
//
// Where this is not what a browser does, each chosen: `body` has no
// margin of its own; a link is not underlined for being one; a heading
// is kept with what follows it; and a distance between lines given in em
// or as a percentage stays a multiple of the text's size for the
// elements inside, as a plain number does.

namespace bn::epub {

// What an element is to the lines of a page.
enum class StyleDisplay : std::uint8_t {
  kNone,      // Nothing: it and all inside it are left out.
  kInline,    // Part of a line.
  kBlock,     // It begins and ends lines of its own.
  kListItem,  // A block that is an item of a list.
};

// To which side an element is floated.
enum class StyleFloat : std::uint8_t { kNone, kLeft, kRight };

// How text is set, as the styles of a document say it of an element. A
// run of a chapter's segment has a style of its own (RunStyle,
// epub/segment.h): its size is of its block's and not of the reader's,
// its language is one of the chapter's by number, and it keeps less of
// what the book said.
struct TextStyle {
  std::uint16_t bits = 0;      // kTextBold and the rest.
  std::uint16_t fromBook = 0;  // The same bits: for each, whether the book said whether it is so.
  CssFamilyClass family = CssFamilyClass::kUnset;
  bool familyFromBook = false;
  // Which language: a number that StyleStack::Language() turns into the
  // tag, 0 for none said.
  std::uint8_t language = 0;
  // Thousandths of the reader's text size, from nothing to 65535.
  std::uint16_t size = 1000;
  bool sizeFromBook = false;

  friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

// The distance from one line to the next.
struct StyleLineHeight {
  enum class Kind : std::uint8_t {
    kUnset,     // Nothing said, or `normal`: the reader's.
    kMultiple,  // So many thousandths of the text's size.
    kLength,
  };
  Kind kind = Kind::kUnset;
  std::uint16_t multiple = 0;  // For kMultiple, held to 65535.
  StyleLength length;          // For kLength.

  friend bool operator==(const StyleLineHeight&, const StyleLineHeight&) = default;
};

// The four sides, as CssProperty has them.
inline constexpr std::size_t kStyleTop = 0;
inline constexpr std::size_t kStyleRight = 1;
inline constexpr std::size_t kStyleBottom = 2;
inline constexpr std::size_t kStyleLeft = 3;

// What an element comes to.
struct ElementStyle {
  StyleDisplay display = StyleDisplay::kInline;

  // How its text is set, and what is set of the lines of a block, which
  // an element has from the one it is inside unless something says
  // otherwise.
  TextStyle text;
  StyleAlign align = StyleAlign::kUnset;
  bool alignFromBook = false;
  bool hasIndent = false;  // Something said how far a first line is set in.
  StyleLength indent;
  bool indentFromBook = false;
  StyleLineHeight lineHeight;
  bool lineHeightFromBook = false;
  StyleMarker marker = StyleMarker::kDisc;
  bool markerFromBook = false;
  bool rightToLeft = false;
  bool spacesAsWritten = false;  // Its white space is kept as it is written (`pre`).

  // What is its own, and no element's inside it: the space around it in
  // CssProperty's order of sides, a margin that is `auto`, its width and
  // height where they are said, the side it is floated to, and where a
  // page may break. For each side, in marginFromBook and paddingFromBook,
  // a bit (the top's the lowest) for whether the book said it.
  std::array<StyleLength, 4> margin{};
  std::array<StyleLength, 4> padding{};
  std::uint8_t marginFromBook = 0;
  std::uint8_t paddingFromBook = 0;
  bool marginLeftAuto = false;
  bool marginRightAuto = false;
  bool hasWidth = false;
  StyleLength width;
  bool hasHeight = false;
  StyleLength height;
  StyleFloat floated = StyleFloat::kNone;
  bool breakBefore = false;   // A page ends before it.
  bool breakAfter = false;    // A page ends after it.
  bool keepWithNext = false;  // No page ends between it and what follows.
  bool keepTogether = false;  // No page ends inside it.
  bool keepWithNextFromBook = false;

  // What a rule set for its first letter (`p::first-letter`), as the text
  // of that letter is to be set; and whether the rule floats the letter,
  // which is how a book asks for a capital that is dropped.
  bool hasFirstLetter = false;
  TextStyle firstLetter;
  bool firstLetterFloated = false;
};

// What a StyleStack counts of what it did not take as written.
enum class StyleLeftOut : std::uint8_t {
  kTooDeep,           // An element opened deeper than there is memory for: it has what the deepest kept has.
  kPageMargin,        // A margin or a padding of `html` or `body`.
  kInheritOfOwn,      // `inherit` for a property that is an element's own.
  kLanguage,          // A language's tag there was no room to keep, or that is longer than one may be.
  kBreakAvoidBefore,  // "No page may end before this": nothing keeps that.
  kNotShown,          // Mathematics, or a note of a `ruby`: nothing, by being what it is, with all inside it.
};

inline constexpr std::size_t kStyleLeftOutKinds = 6;

// "an element too deep", "a page margin", ...
const char* StyleLeftOutName(StyleLeftOut kind) noexcept;

// "none", "inline", "block", "list-item"; "unset", "left", ...; "none",
// "disc", ... as CSS has them.
const char* StyleDisplayName(StyleDisplay display) noexcept;
const char* StyleAlignName(StyleAlign align) noexcept;
const char* StyleMarkerName(StyleMarker marker) noexcept;

// Goes through a document's elements and says what each comes to.
//
// Begin() once for a document; then Open() for each element as its parser
// tells of it, with what a cascade said of it, and Close() as it ends.
//
// One call goes once through the element's attributes, and through the
// languages that are kept if the element names one, and does a fixed
// amount of work besides: nothing in a document or a sheet can make it
// do more than MostWorkFor() says.
class StyleStack final {
 public:
  // What is kept of one open element.
  static constexpr std::size_t kLevelBytes = 28;
  // The most languages a document's elements may name between them, and
  // the longest a tag may be; and what keeping them takes.
  static constexpr std::size_t kMaxLanguages = 15;
  static constexpr std::size_t kMaxLanguageBytes = 23;
  static constexpr std::size_t kLanguagesBytes = kMaxLanguages * (kMaxLanguageBytes + 1);

  // `memory` is for the elements that are open, kLevelBytes each, after
  // kLanguagesBytes for the languages' tags: Levels() of them fit. An
  // element opened deeper than that takes what the deepest that is kept
  // gives, as if it were straight inside that one, and is counted; with
  // room for none, every element is as one inside nothing, and no
  // language is kept.
  explicit StyleStack(std::span<std::uint8_t> memory) noexcept;

  StyleStack(const StyleStack&) = delete;
  StyleStack& operator=(const StyleStack&) = delete;
  ~StyleStack() = default;

  // A document begins: no element is open, and nothing is counted.
  void Begin() noexcept;

  // An element opens. `style` is what a cascade said of it and
  // `firstLetter` what it said of its first letter; `said` is made what
  // the element comes to.
  void Open(std::string_view name, const XmlAttributes& attributes, const CssStyle& style, const CssStyle& firstLetter,
            ElementStyle& said) noexcept;
  // The element opened last, and not yet closed, closes.
  void Close() noexcept;

  // How many elements are open, and how many can be kept.
  std::size_t Depth() const noexcept { return depth_; }
  std::size_t Levels() const noexcept { return levels_; }

  // What the open element numbered `index`, the outermost being 0, passes
  // to the elements inside it, as Open() said it of that element: how its
  // text is set, what is set of a block's lines, what stands before the
  // items of a list, the direction and the white space. The rest of `said`
  // is left as it is. For an element deeper than is kept, what the deepest
  // kept passes; for one that is not open, or with none kept, what an
  // element inside nothing has.
  void Passes(std::size_t index, ElementStyle& said) const noexcept;
  // How many lists that element is inside, itself among them if it is
  // one; 255 at the most.
  std::uint8_t Lists(std::size_t index) const noexcept;

  // The tag of the language with this number, as it was first written; an
  // empty one for 0 or a number there is not.
  std::string_view Language(std::uint8_t language) const noexcept;
  std::size_t Languages() const noexcept { return languages_; }

  // The most work one Open() does for an element with this many
  // attributes: an attribute looked at, or a kept language's tag held
  // against the one the element names, is one piece of work.
  static constexpr std::uint32_t MostWorkFor(std::size_t attributes) noexcept {
    return static_cast<std::uint32_t>(attributes + kMaxLanguages);
  }
  // The work the last Open() did.
  std::uint32_t Work() const noexcept { return work_; }

  // How many times each kind of thing was not taken as written, since
  // Begin().
  std::uint32_t LeftOut(StyleLeftOut kind) const noexcept { return leftOut_[static_cast<std::size_t>(kind)]; }

 private:
  struct Level;
  struct Working;

  Level Load(std::size_t index) const noexcept;
  void Store(std::size_t index, const Level& level) noexcept;
  // The level of the element that the one opening is inside, as far as
  // one is kept.
  Level Outside() const noexcept;
  // The level of the open element numbered `index`, as Passes() has it.
  Level At(std::size_t index) const noexcept;
  std::uint8_t LanguageOf(std::string_view tag, std::uint8_t outside) noexcept;
  void Count(StyleLeftOut kind) noexcept { ++leftOut_[static_cast<std::size_t>(kind)]; }

  std::uint8_t* tags_ = nullptr;    // The languages' tags, each with its length before it.
  std::uint8_t* memory_ = nullptr;  // The levels.
  std::size_t levels_ = 0;
  std::size_t depth_ = 0;
  std::size_t languages_ = 0;
  std::uint16_t rootSize_ = 1000;  // The size of the document's own element, which a `rem` is of.
  // How many elements were open when one too deep to keep turned out to
  // be nothing, which hides all inside it; kNotHidden when none has.
  static constexpr std::size_t kNotHidden = static_cast<std::size_t>(-1);
  std::size_t hiddenFrom_ = kNotHidden;
  std::uint32_t work_ = 0;
  std::array<std::uint32_t, kStyleLeftOutKinds> leftOut_{};
};

}  // namespace bn::epub
