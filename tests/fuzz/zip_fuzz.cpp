// The fuzz target for the ZIP reader (epub/zip.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes of an entry are asked for at a call
//   the rest a ZIP file, or what passes for one
//
// The file is opened, its directory is gone through from end to end, and
// the first few entries are each looked for by name and read to the end,
// and then opened again as a book's files are (epub/book_files.h): from
// their places and by their names. What has to hold, whatever the bytes
// are:
//
//   - every call ends, with a status that call can give;
//   - opening the file costs at most nineteen reads of storage, an entry
//     of the directory two, and opening an entry two;
//   - a call that reads from an entry reads no more from storage than a
//     call of its size may (MostStorageFor() in fuzz_book.h);
//   - the directory has no more entries than the file says it has, and
//     exactly that many if it could be gone through;
//   - a name that the directory was seen to have is never "not found";
//   - an entry gives no more bytes than the directory says it holds; one
//     that is read to its end and called sound gave exactly that many, with
//     the checksum the directory has for them (as zlib works it out); and
//     one that failed stays failed;
//   - a file opened from its place has the path its own header gives, as
//     much of it as there is room for, and is opened if and only if that
//     path could be read whole and the entry can be opened; and whether a
//     book's file was opened is what its status says.

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "epub/book_files.h"
#include "epub/zip.h"
#include "fuzz_book.h"
#include "fuzz_support.h"

namespace {

using bn::epub::ZipArchive;
using bn::epub::ZipEntry;
using bn::epub::ZipStatus;

// How many of a file's entries are looked for and read. The rest are only
// gone through.
constexpr std::size_t kEntriesRead = 8;
// The longest name that is kept to look an entry up by.
constexpr std::size_t kLongestName = 256;

constexpr std::array<std::size_t, 8> kPieces = {1, 2, 7, 100, 1000, 1024, 4096, 5000};

struct Kept {
  ZipEntry entry;
  std::array<char, kLongestName> name{};
};

// Reads one entry to its end, or to where it fails.
void ReadEntry(bn::fuzz::Book& book, const ZipEntry& entry, std::size_t pieceBytes) {
  std::size_t mark = book.Mark();
  const ZipStatus opened = book.reader.Open(book.entries, entry);
  BN_FUZZ_CHECK(book.ReadsSince(mark) <= 2, "opening an entry cost more than two reads of storage");
  BN_FUZZ_CHECK(opened == ZipStatus::kOk || opened == ZipStatus::kDamaged || opened == ZipStatus::kUnsupported,
                "opening an entry gave a status it cannot give here");
  BN_FUZZ_CHECK(book.reader.Status() == opened, "an entry's reader does not say what opening it said");
  std::vector<std::uint8_t> piece(pieceBytes);
  if (opened != ZipStatus::kOk) {
    BN_FUZZ_CHECK(book.reader.Read(piece) == bn::core::ByteSource::kError, "an entry that was refused was read");
    return;
  }

  std::uint32_t total = 0;
  uLong checksum = crc32(0L, Z_NULL, 0);
  for (;;) {
    BN_FUZZ_CHECK(book.reader.Remaining() == entry.size - total,
                  "an entry's count of what is left is not what is left");
    mark = book.Mark();
    const std::ptrdiff_t read = book.reader.Read(piece);
    BN_FUZZ_CHECK(book.BytesSince(mark) <= bn::fuzz::MostStorageFor(pieceBytes),
                  "one call on an entry read more from storage than a call of its size may");
    if (read > 0) {
      BN_FUZZ_CHECK(static_cast<std::size_t>(read) <= pieceBytes, "a call gave more than it was asked for");
      BN_FUZZ_CHECK(static_cast<std::size_t>(read) <= entry.size - total,
                    "an entry gave more than the directory says it holds");
      total += static_cast<std::uint32_t>(read);
      checksum = crc32(checksum, piece.data(), static_cast<uInt>(read));
      continue;
    }
    if (read == 0) {
      BN_FUZZ_CHECK(book.reader.Finished() && book.reader.Status() == ZipStatus::kOk,
                    "an entry gave nothing more without having been read to its end and found sound");
      BN_FUZZ_CHECK(total == entry.size, "an entry was called sound without giving all the directory says it holds");
      BN_FUZZ_CHECK(static_cast<std::uint32_t>(checksum) == entry.crc32,
                    "an entry was called sound whose bytes do not have the directory's checksum");
    } else {
      BN_FUZZ_CHECK(read == bn::core::ByteSource::kError, "a call gave back a count below zero");
      BN_FUZZ_CHECK(!book.reader.Finished() && book.reader.Status() != ZipStatus::kOk &&
                        book.reader.Status() != ZipStatus::kEnd && book.reader.Status() != ZipStatus::kNotFound &&
                        book.reader.Status() != ZipStatus::kNotOpen && book.reader.Status() != ZipStatus::kNoWindow,
                    "an entry that failed does not say how, or says something that cannot be");
    }
    // And it stays as it ended.
    const ZipStatus ended = book.reader.Status();
    BN_FUZZ_CHECK(book.reader.Read(piece) == read && book.reader.Status() == ended, "an entry that had ended went on");
    return;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const std::size_t pieceBytes = kPieces[input.Byte() & 7U];
  bn::fuzz::Book book(input.Rest());

  ZipArchive archive;
  std::size_t mark = book.Mark();
  const ZipStatus opened = archive.Open(book.directory);
  BN_FUZZ_CHECK(book.ReadsSince(mark) <= 19, "opening a ZIP file cost more than nineteen reads of storage");
  ZipArchive::Cursor cursor;
  ZipEntry entry;
  std::array<char, kLongestName> name{};
  if (opened != ZipStatus::kOk) {
    BN_FUZZ_CHECK(opened == ZipStatus::kNotZip || opened == ZipStatus::kDamaged || opened == ZipStatus::kUnsupported,
                  "opening a ZIP file gave a status it cannot give here");
    BN_FUZZ_CHECK(!archive.IsOpen() && archive.EntryCount() == 0, "a file that was refused is open");
    BN_FUZZ_CHECK(archive.Next(cursor, entry, name) == ZipStatus::kNotOpen &&
                      archive.Find("mimetype", entry) == ZipStatus::kNotOpen,
                  "a file that was refused answers as if it were open");
    return 0;
  }

  // Through the directory, keeping the first few entries that can be read.
  std::vector<Kept> kept;
  std::uint32_t seen = 0;
  ZipStatus walked = ZipStatus::kOk;
  for (;;) {
    mark = book.Mark();
    walked = archive.Next(cursor, entry, name);
    BN_FUZZ_CHECK(book.ReadsSince(mark) <= 2, "one entry of the directory cost more than two reads of storage");
    if (walked != ZipStatus::kOk && walked != ZipStatus::kUnsupported) {
      break;
    }
    ++seen;
    BN_FUZZ_CHECK(seen <= archive.EntryCount(), "the directory gave more entries than the file says it has");
    if (walked == ZipStatus::kOk && entry.nameLength <= name.size() && kept.size() < kEntriesRead) {
      kept.push_back({.entry = entry, .name = name});
    }
  }
  BN_FUZZ_CHECK(walked == ZipStatus::kEnd || walked == ZipStatus::kDamaged,
                "going through the directory gave a status it cannot give here");
  if (walked == ZipStatus::kEnd) {
    BN_FUZZ_CHECK(seen == archive.EntryCount(), "the directory ended before it had given every entry");
    BN_FUZZ_CHECK(archive.Next(cursor, entry, name) == ZipStatus::kEnd, "a directory that had ended went on");
  }

  for (const Kept& one : kept) {
    const std::string_view wanted(one.name.data(), one.entry.nameLength);
    ZipEntry found;
    const ZipStatus status = archive.Find(wanted, found);
    // It is there, since it was seen. What can stand between the search
    // and it is a record that cannot be read, or an entry before it of
    // the same name that cannot.
    BN_FUZZ_CHECK(status == ZipStatus::kOk || status == ZipStatus::kDamaged || status == ZipStatus::kUnsupported,
                  "a name the directory has was looked for and not found");
    if (status == ZipStatus::kOk) {
      BN_FUZZ_CHECK(found.nameLength == wanted.size(), "an entry was found by a name of another length");
    }

    std::array<char, kLongestName> local{};
    std::size_t length = 0;
    const ZipStatus named = bn::epub::ReadZipEntryName(book.entries, one.entry, local, length);
    BN_FUZZ_CHECK(named == ZipStatus::kOk || named == ZipStatus::kDamaged,
                  "reading an entry's own name gave a status it cannot give here");
    (void)bn::fuzz::Touch(std::string_view(local.data(), std::min(length, local.size())));

    ReadEntry(book, one.entry, pieceBytes);

    // The same entry as a book's file, opened from its place: with room
    // for a path of 40 bytes, so that some are too long for it.
    const ZipStatus alone = book.reader.Open(book.entries, one.entry);
    bn::epub::ZipBookFiles files(archive, book.entries, book.reader);
    std::array<char, 40> path{};
    std::size_t pathLength = SIZE_MAX;
    const bn::core::ByteSource* const file = files.OpenAt(one.entry, path, pathLength);
    BN_FUZZ_CHECK(pathLength <= path.size(), "a file's path is said to be longer than the room given for it");
    if (named == ZipStatus::kOk) {
      BN_FUZZ_CHECK(pathLength == std::min(length, path.size()) &&
                        std::string_view(path.data(), pathLength) ==
                            std::string_view(local.data(), std::min({length, local.size(), path.size()})),
                    "a file opened from its place has another path than its own header gives");
    } else {
      BN_FUZZ_CHECK(pathLength == 0, "a file whose path could not be read has one");
    }
    BN_FUZZ_CHECK((file != nullptr) == (named == ZipStatus::kOk && length <= path.size() && alone == ZipStatus::kOk),
                  "a file was opened from its place that cannot be, or not opened that can");
    BN_FUZZ_CHECK((file != nullptr) == (files.Status() == ZipStatus::kOk),
                  "whether a file was opened from its place is not what the status says");
    BN_FUZZ_CHECK(file == nullptr || (file == &book.reader && book.reader.Remaining() == one.entry.size),
                  "a file opened from its place is not the entry at its start");
    // And by its name, which finds an entry of that name: this one or
    // another.
    const bn::core::ByteSource* const byName = files.Open(wanted);
    BN_FUZZ_CHECK((byName != nullptr) == (files.Status() == ZipStatus::kOk),
                  "whether a file was opened by its name is not what the status says");
    BN_FUZZ_CHECK(byName != nullptr || files.Status() != ZipStatus::kNotFound || status != ZipStatus::kOk,
                  "a file whose name was found a moment ago is said not to be there");
  }
  return 0;
}
