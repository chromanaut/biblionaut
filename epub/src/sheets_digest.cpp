#include "epub/sheets_digest.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "core/checksum.h"

namespace bn::epub {

namespace {

// `checksum` with `text` taken into it.
std::uint32_t With(std::uint32_t checksum, std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  return core::Crc32({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, checksum);
}

// And with a number, written in decimal.
std::uint32_t WithNumber(std::uint32_t checksum, std::uint32_t value) noexcept {
  std::array<char, 10> digits{};
  std::size_t first = digits.size();
  do {
    digits[--first] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  return With(checksum, std::string_view(digits.data() + first, digits.size() - first));
}

}  // namespace

void SheetsDigest::Reset() noexcept {
  checksum_ = 0;
  documents_ = 0;
  unopened_ = 0;
  given_ = 0;
  withOwnStyle_ = 0;
  leftOut_ = {};
}

void SheetsDigest::Line(std::string_view mark, std::string_view text) noexcept {
  checksum_ = With(With(With(checksum_, mark), text), "\n");
}

void SheetsDigest::AddUnopened(std::string_view path) noexcept {
  ++documents_;
  ++unopened_;
  Line("D ", path);
  Line("N", {});
}

void SheetsDigest::Add(std::string_view path, const DocumentSheets& sheets) noexcept {
  if (sheets.Status() != SheetsStatus::kOk) {
    AddUnopened(path);
    return;
  }
  ++documents_;
  Line("D ", path);
  const std::span<const std::uint8_t> list = sheets.Sheets();
  for (std::size_t i = 0; i < list.size(); ++i) {
    const bool own = sheets.HasOwnStyle() && i + 1 == list.size();
    Line("S ", own ? std::string_view("<style>") : sheets.PathOf(list[i]));
  }
  given_ += list.size();
  withOwnStyle_ += sheets.HasOwnStyle() ? 1 : 0;
  // What was left out for it, kind by kind.
  for (std::size_t kind = 0; kind < leftOut_.size(); ++kind) {
    const auto what = static_cast<SheetLeftOut>(kind);
    const std::uint32_t now = sheets.LeftOut(what);
    if (now != leftOut_[kind]) {
      checksum_ = With(With(With(checksum_, "X "), SheetLeftOutName(what)), " ");
      checksum_ = With(WithNumber(checksum_, now - leftOut_[kind]), "\n");
      leftOut_[kind] = now;
    }
  }
}

}  // namespace bn::epub
