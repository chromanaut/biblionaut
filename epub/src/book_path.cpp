#include "epub/book_path.h"

#include <array>

namespace bn::epub {

namespace {

// The value of a hexadecimal digit, or -1.
int HexValue(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

// Whether `reference` starts the way a reference to somewhere else does:
// with a scheme and a colon ("https:", "mailto:"), or with "//".
bool IsElsewhere(std::string_view reference) noexcept {
  if (reference.starts_with("//")) {
    return true;
  }
  for (std::size_t i = 0; i < reference.size(); ++i) {
    const char ch = reference[i];
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool other = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
    if (ch == ':') {
      return i > 0;
    }
    if (!letter && (i == 0 || !other)) {
      return false;
    }
  }
  return false;
}

// Builds a path a part at a time.
class PathBuilder {
 public:
  explicit PathBuilder(std::span<char> out) noexcept : out_(out) {}

  // Takes one part of a path: a name, "." or "..".
  void Part(std::string_view part) noexcept {
    if (part.empty() || part == ".") {
      return;
    }
    if (part == "..") {
      // Back to before the last '/', or to the top.
      while (length_ > 0 && out_[length_ - 1] != '/') {
        --length_;
      }
      if (length_ > 0) {
        --length_;
      }
      return;
    }
    const std::size_t needed = part.size() + (length_ > 0 ? 1 : 0);
    if (needed > out_.size() - length_) {
      overflowed_ = true;
      return;
    }
    if (length_ > 0) {
      out_[length_++] = '/';
    }
    for (const char ch : part) {
      out_[length_++] = ch;
    }
  }

  // Takes a run of parts with '/' between them.
  void Parts(std::string_view parts) noexcept {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= parts.size(); ++i) {
      if (i == parts.size() || parts[i] == '/') {
        Part(std::string_view(parts.data() + start, i - start));
        start = i + 1;
      }
    }
  }

  std::size_t Length() const noexcept { return overflowed_ ? 0 : length_; }

 private:
  std::span<char> out_;
  std::size_t length_ = 0;
  bool overflowed_ = false;
};

}  // namespace

std::size_t ResolveBookPath(std::string_view base, std::string_view reference, std::span<char> path) noexcept {
  // Only the part that names a file.
  for (std::size_t i = 0; i < reference.size(); ++i) {
    if (reference[i] == '#' || reference[i] == '?') {
      reference.remove_suffix(reference.size() - i);
      break;
    }
  }
  if (reference.empty() || IsElsewhere(reference)) {
    return 0;
  }

  PathBuilder builder(path);
  if (reference.front() != '/') {
    builder.Parts(base);
  }
  // The reference a part at a time, each with its %XX turned into bytes
  // first, so that a part which decodes to ".." is treated as one.
  std::size_t start = 0;
  for (std::size_t i = 0; i <= reference.size(); ++i) {
    if (i < reference.size() && reference[i] != '/') {
      continue;
    }
    const std::string_view raw(reference.data() + start, i - start);
    start = i + 1;
    if (raw.find('%') == std::string_view::npos) {
      builder.Part(raw);
      continue;
    }
    std::array<char, 256> decoded{};
    std::size_t length = 0;
    for (std::size_t k = 0; k < raw.size(); ++k) {
      char ch = raw[k];
      if (ch == '%' && k + 2 < raw.size() && HexValue(raw[k + 1]) >= 0 && HexValue(raw[k + 2]) >= 0) {
        ch = static_cast<char>((HexValue(raw[k + 1]) * 16) + HexValue(raw[k + 2]));
        k += 2;
      }
      if (length == decoded.size()) {
        return 0;  // One name longer than any a book has.
      }
      decoded[length++] = ch;
    }
    builder.Part(std::string_view(decoded.data(), length));
  }
  return builder.Length();
}

std::string_view BookDirectory(std::string_view path) noexcept {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  path.remove_suffix(path.size() - slash - 1);
  return path;
}

std::uint64_t NameHash(std::string_view name) noexcept {
  // FNV-1a, 64 bits, folded to 48.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char ch : name) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  return (hash ^ (hash >> 48U)) & 0xFFFFFFFFFFFFULL;
}

}  // namespace bn::epub
