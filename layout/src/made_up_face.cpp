#include "layout/made_up_face.h"

#include <algorithm>

namespace bn::layout {

std::uint16_t MadeUpFace::CharacterWidth(char first, std::uint16_t style) noexcept {
  const auto byte = static_cast<std::uint8_t>(first);
  if ((byte & 0xC0U) == 0x80U || byte == 0xCC || byte == 0xCD) {
    return 0;  // It goes on with a character, or is a mark set over one.
  }
  constexpr std::string_view kNarrow = "iljtf.,;:'!|";
  constexpr std::string_view kWide = "mwMW";
  std::uint16_t width = 36;
  if (kNarrow.find(first) != std::string_view::npos) {
    width = 18;
  } else if (kWide.find(first) != std::string_view::npos) {
    width = 54;
  } else if (first >= 'A' && first <= 'Z') {
    width = 44;
  } else if (first == '-') {
    width = 22;
  } else if (byte >= 0xC0) {
    width = 40;
  }
  return static_cast<std::uint16_t>(width + (4 * style));
}

void MadeUpFace::Measure(std::string_view text, std::uint16_t style, std::span<const std::uint8_t> ends,
                         std::span<std::uint16_t> widths, std::span<std::uint16_t> hyphens) noexcept {
  std::fill(hyphens.begin(), hyphens.end(), HyphenWidth(style));
  bytesMeasured_ += text.size();
  std::size_t from = 0;
  const std::size_t count = std::min(ends.size(), widths.size());
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t width = 0;
    for (; from < ends[i] && from < text.size(); ++from) {
      width += CharacterWidth(text[from], style);
    }
    widths[i] = static_cast<std::uint16_t>(std::min<std::uint32_t>(width, 0xFFFF));
  }
}

std::uint16_t MadeUpFace::SpaceWidth(std::uint16_t style) noexcept {
  return static_cast<std::uint16_t>(18 + (4 * style));
}

std::uint16_t MadeUpFace::HyphenWidth(std::uint16_t style) noexcept {
  return static_cast<std::uint16_t>(22 + (4 * style));
}

}  // namespace bn::layout
