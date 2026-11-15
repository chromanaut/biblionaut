#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/inflate.h"
#include "epub/xml.h"
#include "epub/zip.h"
#include "storage_fakes.h"
#include "zip_builder.h"

// For the targets that read a book: a file in storage that is opened the
// way the device opens a book, and the count of what is read from it.

namespace bn::fuzz {

// The blocks a file is read by, as on the device.
inline constexpr std::size_t kBlockBytes = 4096;

// The most that may be read from storage by one call that takes up to
// `wanted` bytes of an entry, in bytes.
//
// The entry's reader takes from the file no more than the inflater takes
// from it: what one call of that size may (Inflater::MostInputFor), and
// once more for the single byte it asks for after the last, to see that
// the data ends where the directory says. Those bytes lie in so many
// blocks, and one more for a start that is part way into a block.
constexpr std::size_t MostStorageFor(std::size_t wanted) noexcept {
  const std::size_t taken = core::Inflater::MostInputFor(wanted) + core::Inflater::MostInputFor(1);
  return (((taken + kBlockBytes - 1) / kBlockBytes) + 1) * kBlockBytes;
}

// What a step of a reading may read from storage when it parses the next
// 1 KB of a file: five blocks.
inline constexpr std::size_t kMostStorageForAStep = MostStorageFor(epub::XmlParser::kChunkBytes);
static_assert(kMostStorageForAStep == 5 * kBlockBytes);

// A file in storage, open twice: once for the ZIP file's directory and
// once for its entries.
class Book {
 public:
  explicit Book(std::span<const std::uint8_t> file) {
    storage_.Put("/book.epub", test::FakeStorage::Bytes(file.begin(), file.end()));
    // The window starts full of something that is no entry's bytes.
    std::fill(window_.begin(), window_.end(), std::uint8_t{0xA5});
    const bool opened = storage_.Start() == core::StorageStatus::kOk &&
                        directory.Open(storage_, "/book.epub", core::OpenMode::kRead) == core::StorageStatus::kOk &&
                        entries.Open(storage_, "/book.epub", core::OpenMode::kRead) == core::StorageStatus::kOk;
    if (!opened) {
      std::abort();  // The storage is the tests' own: it does not fail.
    }
  }

  // How many reads of storage have been made, to give back to the two
  // below.
  std::size_t Mark() const noexcept { return storage_.reads.size(); }
  // Reads of storage since `mark`, and the bytes they asked for.
  std::size_t ReadsSince(std::size_t mark) const noexcept { return storage_.reads.size() - mark; }
  std::size_t BytesSince(std::size_t mark) const noexcept {
    std::size_t bytes = 0;
    for (std::size_t i = mark; i < storage_.reads.size(); ++i) {
      bytes += storage_.reads[i].size;
    }
    return bytes;
  }

 private:
  test::FakeStorage storage_;
  std::array<std::uint8_t, kBlockBytes> directoryBuffer_{};
  std::array<std::uint8_t, kBlockBytes> entriesBuffer_{};
  std::vector<std::uint8_t> window_ = std::vector<std::uint8_t>(core::Inflater::kWindowBytes);

 public:
  core::BlockFile directory{directoryBuffer_};
  core::BlockFile entries{entriesBuffer_};
  epub::ZipEntryReader reader{window_};
};

// A file of a book that a target makes: its name and what it holds.
using BookFile = std::pair<std::string, std::vector<std::uint8_t>>;

inline std::vector<std::uint8_t> BytesOfText(std::string_view text) { return {text.begin(), text.end()}; }

// A sound ZIP file of `files`, in that order or the other way round, each
// compressed or not, with or without a comment after its end. A target
// that is after what a reader makes of a file inside a book puts its bytes
// in one of these: in a ZIP file that was itself made up of the bytes,
// nearly every change would be stopped at the entry's checksum, and
// nothing after the checksum would be reached.
inline std::vector<std::uint8_t> MakeBook(const std::vector<BookFile>& files, bool deflated, bool reversed,
                                          bool withComment) {
  test::ZipBuilder builder;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const BookFile& file = files[reversed ? files.size() - 1 - i : i];
    builder.Add(file.first, file.second, {.deflate = deflated});
  }
  return builder.Finish(withComment ? BytesOfText("made by a fuzz target, to be read by anything")
                                    : std::vector<std::uint8_t>());
}

}  // namespace bn::fuzz
