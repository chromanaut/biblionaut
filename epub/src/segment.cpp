#include "epub/segment.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "segment_format.h"

namespace bn::epub {

namespace {

using segment_format::Get16;
using segment_format::Get32;
using segment_format::Put16;
using segment_format::Put32;

constexpr std::uint8_t kMostAlign = 5;
constexpr std::uint8_t kMostDirection = 1;
constexpr std::size_t kMasks = 4;

void PutLength(std::span<std::uint8_t> bytes, std::size_t& at, const StyleLength& length) noexcept {
  Put16(bytes, at, static_cast<std::uint16_t>(length.em));
  Put16(bytes, at + 2, static_cast<std::uint16_t>(length.px));
  Put16(bytes, at + 4, static_cast<std::uint16_t>(length.percent));
  at += 6;
}

StyleLength GetLength(std::span<const std::uint8_t> bytes, std::size_t& at) noexcept {
  StyleLength length;
  length.em = static_cast<std::int16_t>(Get16(bytes, at));
  length.px = static_cast<std::int16_t>(Get16(bytes, at + 2));
  length.percent = static_cast<std::int16_t>(Get16(bytes, at + 4));
  at += 6;
  return length;
}

// How many bytes the fields of a style record come to.
std::size_t FieldBytes(std::uint16_t has) noexcept {
  std::size_t bytes = 0;
  bytes += (has & kBlockAlign) != 0 ? 1U : 0U;
  bytes += (has & kBlockDirection) != 0 ? 1U : 0U;
  bytes += (has & kBlockSize) != 0 ? 2U : 0U;
  bytes += (has & kBlockLineHeight) != 0 ? 8U : 0U;
  bytes += (has & kBlockIndent) != 0 ? 6U : 0U;
  bytes += (has & kBlockMarginLeft) != 0 ? 6U : 0U;
  bytes += (has & kBlockMarginRight) != 0 ? 6U : 0U;
  bytes += (has & kBlockSpaceBefore) != 0 ? 6U : 0U;
  bytes += (has & kBlockSpaceAfter) != 0 ? 6U : 0U;
  bytes += (has & kBlockBreaking) != 0 ? 2U : 0U;
  bytes += (has & kBlockDepths) != 0 ? 2U : 0U;
  bytes += (has & kBlockMarker) != 0 ? 5U : 0U;
  bytes += (has & kBlockTable) != 0 ? 10U : 0U;
  bytes += (has & kBlockWidth) != 0 ? 6U : 0U;
  bytes += (has & kBlockHeight) != 0 ? 6U : 0U;
  return bytes;
}

}  // namespace

const char* SegmentStatusName(SegmentStatus status) noexcept {
  switch (status) {
    case SegmentStatus::kOk:
      return "ok";
    case SegmentStatus::kEnd:
      return "end";
    case SegmentStatus::kMore:
      return "more";
    case SegmentStatus::kDamaged:
      return "damaged";
    case SegmentStatus::kStoreFailed:
      return "store failed";
    case SegmentStatus::kTooBig:
      return "too big";
    case SegmentStatus::kMisused:
      return "misused";
  }
  return "?";
}

const char* SegmentBlockKindName(std::uint8_t kind) noexcept {
  switch (kind) {
    case static_cast<std::uint8_t>(SegmentBlockKind::kParagraph):
      return "paragraph";
    case static_cast<std::uint8_t>(SegmentBlockKind::kHeading):
      return "heading";
    case static_cast<std::uint8_t>(SegmentBlockKind::kListItem):
      return "list-item";
    case static_cast<std::uint8_t>(SegmentBlockKind::kPicture):
      return "picture";
    case static_cast<std::uint8_t>(SegmentBlockKind::kRule):
      return "rule";
    case static_cast<std::uint8_t>(SegmentBlockKind::kTableCell):
      return "table-cell";
    default:
      return "?";
  }
}

std::size_t StoreBlockStyle(const BlockStyle& style, std::span<std::uint8_t, kBlockStyleMostBytes> out) noexcept {
  const auto has = static_cast<std::uint16_t>(style.has & kBlockFields);
  if (has == 0) {
    return 0;
  }
  const std::span<std::uint8_t> bytes = out;
  Put16(bytes, 0, has);
  Put16(bytes, 2, static_cast<std::uint16_t>(style.fromBook & has));
  std::size_t at = kMasks;
  if ((has & kBlockAlign) != 0) {
    bytes[at++] = style.align;
  }
  if ((has & kBlockDirection) != 0) {
    bytes[at++] = style.direction;
  }
  if ((has & kBlockSize) != 0) {
    Put16(bytes, at, style.size);
    at += 2;
  }
  if ((has & kBlockLineHeight) != 0) {
    Put16(bytes, at, style.lineMultiple);
    at += 2;
    PutLength(bytes, at, style.lineHeight);
  }
  if ((has & kBlockIndent) != 0) {
    PutLength(bytes, at, style.indent);
  }
  if ((has & kBlockMarginLeft) != 0) {
    PutLength(bytes, at, style.marginLeft);
  }
  if ((has & kBlockMarginRight) != 0) {
    PutLength(bytes, at, style.marginRight);
  }
  if ((has & kBlockSpaceBefore) != 0) {
    PutLength(bytes, at, style.spaceBefore);
  }
  if ((has & kBlockSpaceAfter) != 0) {
    PutLength(bytes, at, style.spaceAfter);
  }
  if ((has & kBlockBreaking) != 0) {
    Put16(bytes, at, style.breaking);
    at += 2;
  }
  if ((has & kBlockDepths) != 0) {
    bytes[at++] = style.quoteDepth;
    bytes[at++] = style.listDepth;
  }
  if ((has & kBlockMarker) != 0) {
    bytes[at++] = style.marker;
    Put32(bytes, at, style.markerNumber);
    at += 4;
  }
  if ((has & kBlockTable) != 0) {
    Put16(bytes, at, style.table);
    Put16(bytes, at + 2, style.row);
    Put16(bytes, at + 4, style.column);
    bytes[at + 6] = style.rowSpan;
    bytes[at + 7] = style.columnSpan;
    bytes[at + 8] = style.tableBits;
    bytes[at + 9] = style.tableDepth;
    at += 10;
  }
  if ((has & kBlockWidth) != 0) {
    PutLength(bytes, at, style.width);
  }
  if ((has & kBlockHeight) != 0) {
    PutLength(bytes, at, style.height);
  }
  return at;
}

bool LoadBlockStyle(std::span<const std::uint8_t> in, BlockStyle& style) noexcept {
  style = BlockStyle{};
  if (in.empty()) {
    return true;
  }
  if (in.size() < kMasks) {
    return false;
  }
  const std::uint16_t has = Get16(in, 0);
  const std::uint16_t fromBook = Get16(in, 2);
  // A record with no field is not written; a field there is none of; a
  // field marked as the book's that is not there; a length that is not
  // what the fields come to.
  if (has == 0 || has > kBlockFields || (fromBook | has) != has || in.size() != kMasks + FieldBytes(has)) {
    return false;
  }
  style.has = has;
  style.fromBook = fromBook;
  std::size_t at = kMasks;
  if ((has & kBlockAlign) != 0) {
    style.align = in[at++];
    if (style.align > kMostAlign) {
      return false;
    }
  }
  if ((has & kBlockDirection) != 0) {
    style.direction = in[at++];
    if (style.direction > kMostDirection) {
      return false;
    }
  }
  if ((has & kBlockSize) != 0) {
    style.size = Get16(in, at);
    at += 2;
  }
  if ((has & kBlockLineHeight) != 0) {
    style.lineMultiple = Get16(in, at);
    at += 2;
    style.lineHeight = GetLength(in, at);
  }
  if ((has & kBlockIndent) != 0) {
    style.indent = GetLength(in, at);
  }
  if ((has & kBlockMarginLeft) != 0) {
    style.marginLeft = GetLength(in, at);
  }
  if ((has & kBlockMarginRight) != 0) {
    style.marginRight = GetLength(in, at);
  }
  if ((has & kBlockSpaceBefore) != 0) {
    style.spaceBefore = GetLength(in, at);
  }
  if ((has & kBlockSpaceAfter) != 0) {
    style.spaceAfter = GetLength(in, at);
  }
  if ((has & kBlockBreaking) != 0) {
    style.breaking = Get16(in, at);
    at += 2;
  }
  if ((has & kBlockDepths) != 0) {
    style.quoteDepth = in[at++];
    style.listDepth = in[at++];
  }
  if ((has & kBlockMarker) != 0) {
    style.marker = in[at++];
    style.markerNumber = Get32(in, at);
    at += 4;
  }
  if ((has & kBlockTable) != 0) {
    style.table = Get16(in, at);
    style.row = Get16(in, at + 2);
    style.column = Get16(in, at + 4);
    style.rowSpan = in[at + 6];
    style.columnSpan = in[at + 7];
    style.tableBits = in[at + 8];
    style.tableDepth = in[at + 9];
    at += 10;
  }
  if ((has & kBlockWidth) != 0) {
    style.width = GetLength(in, at);
  }
  if ((has & kBlockHeight) != 0) {
    style.height = GetLength(in, at);
  }
  return true;
}

}  // namespace bn::epub
