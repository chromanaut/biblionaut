#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bn::draw {

// What a style sheet held that was not read. Dropped() has one bit for
// each kind met.
enum class SvgSheetDropped : std::uint8_t {
  kSelector = 1U << 0U,  // A selector other than a class or an element's name: its rule is passed over.
  kAtRule = 1U << 1U,    // An `@` rule, such as `@media` or `@import`: passed over, with its block.
  kTooLong = 1U
      << 2U,  // Selectors, or a declaration, too long to hold: that rule, or that declaration, is passed over.
  kUnended = 1U << 3U,  // The sheet ended inside a rule, a comment or a string: what was not finished is lost.
};

// What a selector picks out.
struct SvgSelector {
  enum class Kind : std::uint8_t {
    kClass,    // `.name`: an element with `name` among its classes.
    kElement,  // `name`: an element of that name.
  };

  Kind kind = Kind::kClass;
  std::string_view name;
};

// One thing a style sheet says.
struct SvgSheetToken {
  enum class Kind : std::uint8_t {
    kNone,         // Nothing yet: the text ran out.
    kSelector,     // A rule begins, or goes on, with this selector. Every selector of a rule comes before any of
                   // its declarations.
    kDeclaration,  // The rule sets `property` to `value`.
    kRuleEnd,      // The rule is over.
  };

  Kind kind = Kind::kNone;
  SvgSelector selector;
  // In small letters, as CSS takes a property's name whatever its case.
  std::string_view property;
  // Without the space round it. As written otherwise, `!important` and all.
  std::string_view value;
};

// Reads the text of an SVG file's own style sheet, the inside of a
// `<style>` element, as much of CSS as the drawings in books use.
//
// Of every sheet inside a drawing in the books, every selector is a class,
// `.st0`, or an element's name, `path`; and what they set is a handful of
// properties. So this reads rules whose selectors are each one of those,
// and gives the declarations of each as they are written; which of them
// matter, and what they come to, is for whoever reads it. A rule with any
// other selector is passed over whole, as CSS passes over a rule with one
// it does not understand, and so is an `@` rule; Dropped() says what was.
//
// The text comes in pieces of any size, cut anywhere. Comments are passed
// over, and so are the `<!--` and `-->` a sheet may be written inside. A
// value may have strings and brackets in it, and a semicolon inside either
// does not end it.
//
// It gives one thing at a call, and holds no more of the sheet than one
// rule's selectors and one declaration: kLongestSelectors bytes, and
// kLongestProperty and kLongestValue. Whatever the text, it ends with a
// report of what it dropped. The object is 284 bytes on the device.
class SvgSheetReader {
 public:
  // The most bytes of selectors before a rule's `{`, of a property's name,
  // and of a value. A rule or a declaration that is longer is passed over.
  static constexpr std::size_t kLongestSelectors = 128;
  static constexpr std::size_t kLongestProperty = 32;
  static constexpr std::size_t kLongestValue = 80;

  // Takes text from the front of `text` until it has something to tell,
  // and says it in `token`. Returns how many bytes it took, which may be
  // none when it has something still to tell from what it took before.
  // Call it until the text is gone and `token` says nothing.
  [[nodiscard]] std::size_t Next(std::string_view text, SvgSheetToken& token) noexcept;

  // Says the sheet has ended. Anything begun and not finished is dropped.
  void Finish() noexcept;

  // A bit of SvgSheetDropped for each kind of thing passed over.
  std::uint8_t Dropped() const noexcept { return dropped_; }

 private:
  enum class State : std::uint8_t {
    kSelectors,  // Before a rule's `{`.
    kTelling,    // Giving out the selectors of a rule whose `{` has come.
    kProperty,   // Inside a rule, reading a property's name.
    kValue,      // After its colon.
    kSkipping,   // Passing over a block, or a statement up to its `;`.
  };

  // What one byte does. True if there is something to tell. Take() sees to
  // comments, and gives Byte() everything else.
  bool Take(char c, SvgSheetToken& token) noexcept;
  bool Byte(char c, SvgSheetToken& token) noexcept;
  // What the selectors before a `{` come to. True if every one can be read.
  bool ReadSelectors() noexcept;
  bool NextSelector(SvgSheetToken& token) noexcept;
  bool EndDeclaration(SvgSheetToken& token) noexcept;
  // Passes over a block, `depth` deep, and then reads selectors again.
  void Skip(std::uint32_t depth) noexcept;
  // Keeps track of a string, in whatever state: true if `c` was in one, or
  // began or ended one.
  bool InString(char c) noexcept;

  State state_ = State::kSelectors;
  State resume_ = State::kSelectors;  // What skipping goes back to.
  std::uint8_t dropped_ = 0;
  // A comment, a string, and what came just before, which may begin or
  // end a comment.
  bool inComment_ = false;
  bool afterSlash_ = false;  // A `/` that may begin a comment, held back.
  bool afterStar_ = false;   // A `*` in a comment that may end it.
  char quote_ = 0;           // Inside a string with this quote; 0 if not.
  bool escaped_ = false;     // After a `\` in a string.
  std::uint32_t depth_ = 0;  // Blocks open while skipping.
  std::uint8_t brackets_ = 0;
  bool ruleEnded_ = false;  // The `}` of a rule has come, and the end of the rule is yet to be told.

  std::array<char, kLongestSelectors> selectors_{};
  std::size_t selectorsSize_ = 0;
  bool selectorsTooLong_ = false;
  std::size_t telling_ = 0;     // How far through the selectors the telling has got,
  std::size_t tellingEnd_ = 0;  // and where they end.

  std::array<char, kLongestProperty> property_{};
  std::size_t propertySize_ = 0;
  std::array<char, kLongestValue> value_{};
  std::size_t valueSize_ = 0;
  bool declarationTooLong_ = false;
};

}  // namespace bn::draw
