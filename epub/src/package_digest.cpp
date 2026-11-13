#include "epub/package_digest.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "core/checksum.h"

namespace bn::epub {

namespace {

// The lines, checksummed as they are written and not kept.
class Lines {
 public:
  Lines& Text(std::string_view text) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
    checksum_ = core::Crc32({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, checksum_);
    return *this;
  }
  Lines& Number(std::uint32_t value) noexcept {
    std::array<char, 10> digits{};
    std::size_t first = digits.size();
    do {
      digits[--first] = static_cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0);
    return Text(std::string_view(digits.data() + first, digits.size() - first));
  }
  Lines& Hex8(std::uint32_t value) noexcept {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::array<char, 8> digits{};
    for (std::size_t i = 0; i < digits.size(); ++i) {
      digits[i] = kDigits[(value >> (28U - (4U * static_cast<unsigned>(i)))) & 0xFU];
    }
    return Text(std::string_view(digits.data(), digits.size()));
  }
  // What the book gave for one of its names, or "-".
  Lines& Said(std::string_view text) noexcept { return Text(text.empty() ? std::string_view("-") : text); }
  Lines& Entry(const ZipEntry& entry) noexcept {
    return Number(entry.headerOffset).Text(" ").Number(entry.size).Text(" ").Hex8(entry.crc32);
  }
  // An entry that may not be there, written as `none` if it is not.
  Lines& EntryOr(const ZipEntry* entry, std::string_view none) noexcept {
    return entry != nullptr ? Entry(*entry) : Text(none);
  }

  std::uint32_t Checksum() const noexcept { return checksum_; }

 private:
  std::uint32_t checksum_ = 0;
};

}  // namespace

std::uint32_t PackageChecksum(const Package& package) noexcept {
  Lines lines;
  lines.Text("title: ").Said(package.Title()).Text("\n");
  lines.Text("author: ").Said(package.Author()).Text("\n");
  lines.Text("language: ").Said(package.Language()).Text("\n");
  lines.Text("package: ").Text(package.PackagePath()).Text("\n");
  lines.Text("manifest: ").Number(package.ManifestItems()).Text("\n");
  lines.Text("cover: ");
  if (package.Cover() != nullptr) {
    lines.Text(MediaTypeName(package.CoverType())).Text(" ").Entry(*package.Cover());
  } else {
    lines.Text("-");
  }
  lines.Text("\nnav: ").EntryOr(package.Nav(), "-");
  lines.Text("\nncx: ").EntryOr(package.Ncx(), "-");
  lines.Text("\nspine: ").Number(static_cast<std::uint32_t>(package.Spine().size())).Text("\n");
  for (const SpineItem& item : package.Spine()) {
    lines.Text(item.linear ? "+ " : "- ")
        .Text(MediaTypeName(item.type))
        .Text(" ")
        .EntryOr(item.present ? &item.entry : nullptr, "?")
        .Text("\n");
  }
  lines.Text("unknown: ").Number(package.UnknownSpineItems()).Text("\n");
  lines.Text("cut short: ").Text(package.CutShort() ? "1" : "0").Text("\n");
  return lines.Checksum();
}

}  // namespace bn::epub
