#include "reader/gray_rows.h"

#include <algorithm>
#include <cstring>

namespace bn::reader {

namespace {

constexpr std::uint32_t kFourWhite = 0xFFFFFFFFU;

// Four pixels' grays as one number: kFourWhite if each is white, nought if
// each is black, whichever end of it the first is at.
std::uint32_t Four(std::span<const std::uint8_t> grays) noexcept {
  std::uint32_t four = 0;
  std::memcpy(&four, grays.data(), sizeof four);
  return four;
}

}  // namespace

bool PackWhite(std::span<const std::uint8_t> grays, std::span<std::uint8_t> bits) noexcept {
  const std::size_t width = std::min(grays.size(), bits.size() * 8);
  const std::size_t whole = width / 8;
  unsigned seen = 0;
  for (std::size_t at = 0; at < whole; ++at) {
    const std::span<const std::uint8_t> eight = grays.subspan(at * 8, 8);
    const std::uint32_t first = Four(eight);
    const std::uint32_t second = Four(eight.subspan(4));
    if ((first & second) == kFourWhite) {
      bits[at] = 0xFF;
      continue;
    }
    if ((first | second) == 0) {
      bits[at] = 0;
      continue;
    }
    unsigned byte = 0;
    for (unsigned pixel = 0; pixel < 8; ++pixel) {
      const unsigned gray = eight[pixel];
      byte |= gray == 255 ? 0x80U >> pixel : 0U;
      seen |= gray != 0 && gray != 255 ? 1U : 0U;
    }
    bits[at] = static_cast<std::uint8_t>(byte);
  }
  if (whole < bits.size()) {
    // The pixels left over, fewer than eight, and nothing after them.
    unsigned byte = 0;
    for (std::size_t x = whole * 8; x < width; ++x) {
      const unsigned gray = grays[x];
      byte |= gray == 255 ? 0x80U >> (x % 8) : 0U;
      seen |= gray != 0 && gray != 255 ? 1U : 0U;
    }
    bits[whole] = static_cast<std::uint8_t>(byte);
    std::fill(bits.begin() + static_cast<std::ptrdiff_t>(whole) + 1, bits.end(), std::uint8_t{0});
  }
  return seen != 0;
}

void PackLevels(std::span<const std::uint8_t> grays, unsigned bits, unsigned top,
                std::span<std::uint8_t> kept) noexcept {
  if (bits == 0 || bits > 8) {
    return;
  }
  // The pixels a byte holds.
  const std::size_t each = 8 / bits;
  const std::size_t width = std::min(grays.size(), kept.size() * each);
  const std::size_t whole = width / each;
  const std::uint8_t white = KeptWhite(bits, top);
  for (std::size_t at = 0; at < whole; ++at) {
    const std::span<const std::uint8_t> group = grays.subspan(at * each, each);
    if (each >= 4) {
      std::uint32_t all = Four(group);
      std::uint32_t any = all;
      if (each == 8) {
        const std::uint32_t second = Four(group.subspan(4));
        all &= second;
        any |= second;
      }
      if (all == kFourWhite) {
        kept[at] = white;
        continue;
      }
      if (any == 0) {
        kept[at] = 0;
        continue;
      }
    }
    unsigned byte = 0;
    for (std::size_t pixel = 0; pixel < each; ++pixel) {
      byte |= LevelOf(group[pixel], top) << (8U - bits - (pixel * bits));
    }
    kept[at] = static_cast<std::uint8_t>(byte);
  }
  if (whole < kept.size()) {
    // The pixels left over, fewer than a byte's, and nothing after them.
    unsigned byte = 0;
    for (std::size_t x = whole * each; x < width; ++x) {
      byte |= LevelOf(grays[x], top) << (8U - bits - ((x - (whole * each)) * bits));
    }
    kept[whole] = static_cast<std::uint8_t>(byte);
    std::fill(kept.begin() + static_cast<std::ptrdiff_t>(whole) + 1, kept.end(), std::uint8_t{0});
  }
}

bool UnpackWhite(std::span<const std::uint8_t> kept, std::size_t width, unsigned bits, unsigned top,
                 std::span<std::uint8_t> white) noexcept {
  if (bits == 0 || bits > 8) {
    return false;
  }
  const unsigned mask = (1U << bits) - 1U;
  const std::uint8_t whiteByte = KeptWhite(bits, top);
  const std::size_t pixels = std::min({width, (kept.size() * 8) / bits, white.size() * 8});
  unsigned seen = 0;
  // Eight pixels are `bits` bytes as they are kept, and one as they come
  // out.
  const std::size_t whole = pixels / 8;
  for (std::size_t at = 0; at < whole; ++at) {
    const std::span<const std::uint8_t> eight = kept.subspan(at * bits, bits);
    if (std::all_of(eight.begin(), eight.end(), [whiteByte](std::uint8_t byte) { return byte == whiteByte; })) {
      white[at] = 0xFF;
      continue;
    }
    if (std::all_of(eight.begin(), eight.end(), [](std::uint8_t byte) { return byte == 0; })) {
      white[at] = 0;
      continue;
    }
    unsigned byte = 0;
    for (std::size_t pixel = 0; pixel < 8; ++pixel) {
      const std::size_t bit = pixel * bits;
      const unsigned level = (static_cast<unsigned>(eight[bit / 8]) >> (8U - bits - (bit % 8))) & mask;
      byte |= level == top ? 0x80U >> pixel : 0U;
      seen |= level != 0 && level != top ? 1U : 0U;
    }
    white[at] = static_cast<std::uint8_t>(byte);
  }
  if (whole < white.size()) {
    unsigned byte = 0;
    for (std::size_t x = whole * 8; x < pixels; ++x) {
      const std::size_t bit = x * bits;
      const unsigned level = (static_cast<unsigned>(kept[bit / 8]) >> (8U - bits - (bit % 8))) & mask;
      byte |= level == top ? 0x80U >> (x % 8) : 0U;
      seen |= level != 0 && level != top ? 1U : 0U;
    }
    white[whole] = static_cast<std::uint8_t>(byte);
    std::fill(white.begin() + static_cast<std::ptrdiff_t>(whole) + 1, white.end(), std::uint8_t{0});
  }
  return seen != 0;
}

}  // namespace bn::reader
