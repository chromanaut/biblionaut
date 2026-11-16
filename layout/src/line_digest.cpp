#include "layout/line_digest.h"

#include <array>
#include <cstddef>
#include <span>

#include "core/checksum.h"

namespace bn::layout {

namespace {

void PutLittleEndian(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t value, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    bytes[at + i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
}

}  // namespace

void LineDigest::Add(const TextLine& line) noexcept {
  std::array<std::uint8_t, 19> bytes{};
  PutLittleEndian(bytes, 0, line.begin, 4);
  PutLittleEndian(bytes, 4, line.end, 4);
  PutLittleEndian(bytes, 8, line.width, 4);
  PutLittleEndian(bytes, 12, line.number, 4);
  PutLittleEndian(bytes, 16, line.spaces, 2);
  bytes[18] = static_cast<std::uint8_t>((line.hyphen ? 1U : 0U) | (line.last ? 2U : 0U) | (line.overfull ? 4U : 0U));
  crc_ = core::Crc32(bytes, crc_);
  ++lines_;
}

}  // namespace bn::layout
