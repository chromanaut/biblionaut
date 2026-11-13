#include "epub/toc_digest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "core/checksum.h"

namespace bn::epub {

namespace {

// Adds `text` to a checksum.
std::uint32_t Add(std::uint32_t checksum, std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  return core::Crc32({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, checksum);
}

// Adds a number, written in decimal.
std::uint32_t AddNumber(std::uint32_t checksum, std::size_t value) noexcept {
  std::array<char, 20> digits{};
  std::size_t first = digits.size();
  do {
    digits[--first] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  return Add(checksum, std::string_view(digits.data() + first, digits.size() - first));
}

}  // namespace

bool TocDigest::OnEntry(const TocEntry& entry) noexcept {
  ++entries_;
  levels_ = std::max(levels_, std::uint32_t{entry.depth} + 1);
  checksum_ = Add(AddNumber(checksum_, entry.depth), "\t");
  if (entry.document == Package::kNotInSpine) {
    ++withoutDocument_;
    checksum_ = Add(checksum_, "-");
  } else {
    checksum_ = AddNumber(checksum_, entry.document);
  }
  checksum_ = Add(checksum_, "\t");
  checksum_ = Add(checksum_, entry.fragment.empty() ? std::string_view("-") : entry.fragment);
  checksum_ = Add(checksum_, "\t");
  checksum_ = Add(checksum_, entry.label);
  checksum_ = Add(checksum_, "\n");
  return true;
}

void TocDigest::Reset() noexcept {
  checksum_ = 0;
  entries_ = 0;
  levels_ = 0;
  withoutDocument_ = 0;
}

}  // namespace bn::epub
