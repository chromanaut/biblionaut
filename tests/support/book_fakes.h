#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "epub/book_files.h"

namespace bn::test {

// A book whose files are held in memory, which counts what is asked of
// it: how many files were opened and which, and how many bytes were read.
class FakeBook final : public epub::BookFiles {
 public:
  static constexpr std::size_t kNever = SIZE_MAX;

  void Put(std::string_view path, std::string_view contents) { files_[std::string(path)] = std::string(contents); }
  bool Has(std::string_view path) const { return files_.find(path) != files_.end(); }
  // What the file at `path` holds; nothing if there is none.
  std::string_view Contents(std::string_view path) const {
    const auto found = files_.find(path);
    return found == files_.end() ? std::string_view{} : std::string_view(found->second);
  }
  // The file at `path` fails once so many of its bytes have been read, as
  // a damaged entry of a ZIP file does. kNever for a sound one.
  void FailAfter(std::string_view path, std::size_t bytes) { failAfter_[std::string(path)] = bytes; }
  std::size_t FailsAfter(std::string_view path) const {
    const auto found = failAfter_.find(path);
    return found == failAfter_.end() ? kNever : found->second;
  }

  [[nodiscard]] core::ByteSource* Open(std::string_view path) noexcept override {
    ++opens;
    opened.emplace_back(path);
    const auto found = files_.find(path);
    if (found == files_.end()) {
      return nullptr;
    }
    source_.Start(*this, found->second, FailsAfter(path));
    return &source_;
  }

  std::size_t opens = 0;
  std::vector<std::string> opened;
  std::uint64_t bytesRead = 0;
  // The most bytes one read gives: fewer than asked, to try a reader on
  // pieces of other sizes than its own.
  std::size_t pieceBytes = kNever;

 private:
  class Source final : public core::ByteSource {
   public:
    void Start(FakeBook& book, std::string_view bytes, std::size_t failAfter) {
      book_ = &book;
      bytes_ = bytes;
      failAfter_ = failAfter;
      at_ = 0;
    }
    [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
      const std::size_t end = std::min(bytes_.size(), failAfter_);
      if (at_ == end && failAfter_ <= bytes_.size()) {
        return kError;
      }
      const std::size_t count = std::min({dst.size(), end - at_, book_->pieceBytes});
      if (count > 0) {
        std::memcpy(dst.data(), bytes_.data() + at_, count);
      }
      at_ += count;
      book_->bytesRead += count;
      return static_cast<std::ptrdiff_t>(count);
    }

   private:
    FakeBook* book_ = nullptr;
    std::string_view bytes_;
    std::size_t failAfter_ = kNever;
    std::size_t at_ = 0;
  };

  std::map<std::string, std::string, std::less<>> files_;
  std::map<std::string, std::size_t, std::less<>> failAfter_;
  Source source_;
};

}  // namespace bn::test
