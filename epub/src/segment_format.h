#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/endian.h"

// Where things are in a chapter's segment, for the sources of its writer
// and its reader.

namespace bn::epub::segment_format {

inline constexpr std::size_t kPage = 4096;
// What begins every record: a kind, a byte for the kind, a length.
inline constexpr std::size_t kHead = 4;

// A block's record in an index page.
inline constexpr std::size_t kIndexRecord = 16;
inline constexpr std::size_t kIndexAtOffset = 0;
inline constexpr std::size_t kIndexAtCharacters = 4;
inline constexpr std::size_t kIndexAtSource = 8;
inline constexpr std::size_t kIndexAtKind = 12;
inline constexpr std::size_t kIndexAtBreaking = 13;

// What an id has in an anchor page besides its name: the name's length
// before it, and the block and the characters into it after.
inline constexpr std::size_t kAnchorOther = 7;

// The directory: what it begins with, and its entries.
inline constexpr std::size_t kDirectoryHead = 20;
inline constexpr std::size_t kDirectoryAtIndexPages = 0;
inline constexpr std::size_t kDirectoryAtAnchorPages = 2;
inline constexpr std::size_t kDirectoryAtPictures = 4;
inline constexpr std::size_t kDirectoryAtLanguages = 6;
inline constexpr std::size_t kDirectoryAtBlocks = 8;
inline constexpr std::size_t kDirectoryAtCharacters = 12;
inline constexpr std::size_t kDirectoryAtSourceBytes = 16;
inline constexpr std::size_t kPageEntry = 12;
inline constexpr std::size_t kPictureEntry = 8;

// The items whose lengths are fixed, and what the others have before
// their text.
inline constexpr std::size_t kEndBytes = 8;
inline constexpr std::size_t kStyleBytes = 8;
inline constexpr std::size_t kLinkBytes = 3;
inline constexpr std::size_t kPictureBytes = 16;

inline std::uint16_t Get16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe16(bytes.subspan(at).first<2>());
}

inline std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe32(bytes.subspan(at).first<4>());
}

inline void Put16(std::span<std::uint8_t> bytes, std::size_t at, std::uint16_t value) noexcept {
  core::StoreLe16(bytes.subspan(at).first<2>(), value);
}

inline void Put32(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t value) noexcept {
  core::StoreLe32(bytes.subspan(at).first<4>(), value);
}

}  // namespace bn::epub::segment_format
