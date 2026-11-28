#pragma once

#include <cstdint>

// What a book's styles come to that is said in one way wherever it is
// said: by the StyleStack of an element (epub/style_stack.h), by
// DocumentBlocks of a block, and in a chapter's segment of a block and of
// a run of text (epub/segment.h). The importer hands these from one to
// the other as they are.

namespace bn::epub {

// A length: three amounts, added together, so that what is in ems, in
// pixels and in percentages can be added up, and nothing that is kept
// depends on the reader's font or on the page. Each is held to what 16
// bits take.
struct StyleLength {
  std::int16_t em = 0;       // 64ths of the reader's text size.
  std::int16_t px = 0;       // 8ths of a CSS pixel.
  std::int16_t percent = 0;  // 64ths of a hundredth of the page's text width.

  bool IsZero() const noexcept { return em == 0 && px == 0 && percent == 0; }
  friend bool operator==(const StyleLength&, const StyleLength&) = default;
};

// How lines stand between a block's sides.
enum class StyleAlign : std::uint8_t { kUnset, kLeft, kRight, kCenter, kJustify, kStart, kEnd };

// The number a block's record in a segment has for it (BlockStyle::align):
// 0 left, 1 right, 2 centre, 3 justified, 4 start, 5 end. Nothing said is
// kept as the side lines start from.
inline constexpr std::uint8_t kMostAlignNumber = 5;
constexpr std::uint8_t AlignNumber(StyleAlign align) noexcept {
  const StyleAlign said = align == StyleAlign::kUnset ? StyleAlign::kStart : align;
  return static_cast<std::uint8_t>(static_cast<unsigned>(said) - 1U);
}
// A segment on a card has these numbers: they do not change.
static_assert(AlignNumber(StyleAlign::kLeft) == 0 && AlignNumber(StyleAlign::kRight) == 1 &&
              AlignNumber(StyleAlign::kCenter) == 2 && AlignNumber(StyleAlign::kJustify) == 3 &&
              AlignNumber(StyleAlign::kStart) == 4 && AlignNumber(StyleAlign::kEnd) == kMostAlignNumber &&
              AlignNumber(StyleAlign::kUnset) == 4);

// What stands before an item of a list. A block's record in a segment has
// the number (BlockStyle::marker), so one is added at the end, and none
// is given another number.
enum class StyleMarker : std::uint8_t {
  kNone = 0,
  kDisc = 1,
  kCircle = 2,
  kSquare = 3,
  kDecimal = 4,
  kDecimalLeadingZero = 5,
  kLowerAlpha = 6,
  kUpperAlpha = 7,
  kLowerRoman = 8,
  kUpperRoman = 9,
};

// How text is set, as bits: of TextStyle::bits, and of RunStyle::bits,
// which a segment's style item keeps as they are here.
inline constexpr std::uint16_t kTextBold = 1U << 0U;
inline constexpr std::uint16_t kTextItalic = 1U << 1U;
inline constexpr std::uint16_t kTextUnderline = 1U << 2U;
inline constexpr std::uint16_t kTextLineThrough = 1U << 3U;
inline constexpr std::uint16_t kTextSmallCaps = 1U << 4U;
inline constexpr std::uint16_t kTextRaised = 1U << 5U;
inline constexpr std::uint16_t kTextLowered = 1U << 6U;
inline constexpr std::uint16_t kTextBits = 0x7FU;

}  // namespace bn::epub
