#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace bn::core {

// A short piece of UTF-8 text kept in a buffer of its own: a title, a
// name. It holds up to N bytes and no more; text that does not fit is cut
// short, always at the end of a character and never inside one.
template <std::size_t N>
class FixedText {
 public:
  constexpr FixedText() noexcept = default;

  std::string_view View() const noexcept { return {bytes_.data(), length_}; }
  std::size_t Size() const noexcept { return length_; }
  bool Empty() const noexcept { return length_ == 0; }
  // Something was left off the end because there was no room for it.
  bool Truncated() const noexcept { return truncated_; }
  static constexpr std::size_t Capacity() noexcept { return N; }

  void Clear() noexcept {
    length_ = 0;
    truncated_ = false;
  }

  // Adds `text` to the end, or as many whole characters of it as fit.
  // Once something has been left off, nothing more is taken.
  void Append(std::string_view text) noexcept {
    if (truncated_) {
      return;
    }
    std::size_t fits = text.size();
    if (fits > N - length_) {
      fits = N - length_;
      // Back to the start of the character that would have been cut: the
      // bytes of a character after its first all begin with the bits 10.
      while (fits > 0 && (static_cast<unsigned char>(text[fits]) & 0xC0U) == 0x80U) {
        --fits;
      }
      truncated_ = true;
    }
    for (std::size_t i = 0; i < fits; ++i) {
      bytes_[length_ + i] = text[i];
    }
    length_ += fits;
  }

  // The same, with every run of spaces, tabs and line ends in `text` made
  // one space, and none kept at the very start. A run that `text` ends
  // with is kept as a space: RemoveTrailingSpace() takes it off when no
  // more is coming.
  void AppendCollapsed(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size() && !truncated_;) {
      if (IsSpace(text[i])) {
        while (i < text.size() && IsSpace(text[i])) {
          ++i;
        }
        if (length_ != 0 && bytes_[length_ - 1] != ' ') {
          Append(" ");
        }
        continue;
      }
      std::size_t end = i;
      while (end < text.size() && !IsSpace(text[end])) {
        ++end;
      }
      Append(std::string_view(text.data() + i, end - i));
      i = end;
    }
  }

  void RemoveTrailingSpace() noexcept {
    while (length_ != 0 && bytes_[length_ - 1] == ' ') {
      --length_;
    }
  }

 private:
  static constexpr bool IsSpace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

  std::array<char, N> bytes_{};
  std::size_t length_ = 0;
  bool truncated_ = false;
};

}  // namespace bn::core
