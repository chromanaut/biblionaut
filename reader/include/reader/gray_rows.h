// Rows of grays that have been made the panel's levels, packed for the
// panel and for the picture cache, and rows of the cache unpacked.
//
// A pixel of such a row is the gray of one of a few levels, from 0 for
// black to 255 for white, and most of a page is white with some black. So
// each of these takes four or eight pixels at once, and passes over those
// that are all white or all black without looking at them one by one.
// What comes out is what looking at each pixel would give.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bn::reader {

// The level of a pixel made one of `top` + 1 levels: its gray is that
// level's, from 0 for black to 255 for white.
constexpr unsigned LevelOf(std::uint8_t gray, unsigned top) noexcept { return ((gray * top) + 127U) / 255U; }

// A byte of a row kept at `bits` bits a pixel (1, 2, 4 or 8) whose pixels
// are all white: the level `top` in each of its places.
constexpr std::uint8_t KeptWhite(unsigned bits, unsigned top) noexcept {
  unsigned byte = 0;
  for (unsigned place = 0; bits != 0 && place < 8; place += bits) {
    byte |= top << place;
  }
  return static_cast<std::uint8_t>(byte & 0xFFU);
}

// `grays` in black and white, a bit a pixel, in `bits`: set where the pixel
// is white, the leftmost pixel the top bit of the first byte, and the bits
// after the last pixel clear. `bits` has (grays.size() + 7) / 8 bytes at
// the least; what it has beyond those is cleared. Says whether any pixel is
// neither white nor black.
bool PackWhite(std::span<const std::uint8_t> grays, std::span<std::uint8_t> bits) noexcept;

// The levels of `grays`, each the gray of one of `top` + 1 levels, in
// `kept` at `bits` bits a pixel (1, 2, 4 or 8), the leftmost pixel in the
// top bits of the first byte: a row as the picture cache keeps it. `kept`
// has ((grays.size() * bits) + 7) / 8 bytes at the least, and the bits
// after the last pixel are clear.
void PackLevels(std::span<const std::uint8_t> grays, unsigned bits, unsigned top,
                std::span<std::uint8_t> kept) noexcept;

// A row the cache kept, `width` pixels of `bits` bits whose white is the
// level `top`, in black and white in `white`, as PackWhite() leaves a row:
// (width + 7) / 8 bytes. Says whether any pixel is neither white nor
// black.
bool UnpackWhite(std::span<const std::uint8_t> kept, std::size_t width, unsigned bits, unsigned top,
                 std::span<std::uint8_t> white) noexcept;

}  // namespace bn::reader
