// The fuzz target for a bundle (epub/bundle.h): a file the card holds,
// opened, read, and added to.
//
// What it is given:
//
//   byte 0   bit 0: the header page's checksum is mended
//            bit 1: the checksum of each slot is mended, where the header
//                   says a slot is and the file is long enough to have it
//            bits 2 and 3: how long a segment is committed afterwards:
//                   none, 1 byte, 512, or 700
//            bits 4 to 7: which chapter it is committed as, of as many as
//                   the bundle has
//   two bytes   how long the file is, in fours of bytes
//   the rest    the file's bytes that are not zero, in pieces: where a
//               piece goes, in fours of bytes (two bytes), how long it is
//               (two bytes), and its bytes. A file longer than was said is
//               as long as its pieces make it.
//
// A bundle is mostly pages of nothing, between a header of a hundred
// bytes, tables, and two slots: given as it is, an input would be sixteen
// kilobytes of which the engine could change little to any purpose.
//
// What has to hold, whatever the bytes are:
//
//   - opening ends in a status, in no more steps than the file has pages
//     twice over and a few, each step one read of storage at the most, of
//     a page at the most, and no write;
//   - a file that does not open leaves nothing open to be asked, and one
//     too short for a header page, or that does not begin as one, is no
//     bundle;
//   - one that opens has a header page that passes its checksum and a slot
//     that does, each worked out here from where the format puts them;
//   - of one that opens: the tables lie one after another from the first
//     page's end, before the slots; the segments begin after the slots
//     and end, on a sector, within the file; every chapter's record is
//     one, with its segment among the segments and its directory inside
//     it; the first and last records of the spine and of the pictures can
//     be read; and each table checks as whole or as damaged, a page of the
//     file a step;
//   - a chapter committed to it is found again when the file is opened
//     afresh, with the next sequence number, the segments ending where
//     that chapter's last sector does, every other record as it was, and
//     the chapters coming to what the bundle said they would before it
//     was closed; and the commit wrote one page of a slot a step, to one
//     slot, and nothing in the place of the header or the tables: with
//     the slot it wrote spoiled, the file opens as the bundle it was
//     before, the other slot having been left as it was.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/block_file.h"
#include "core/checksum.h"
#include "core/storage.h"
#include "epub/bundle.h"
#include "fuzz_support.h"
#include "storage_fakes.h"

namespace {

using bn::core::BlockFile;
using bn::core::OpenMode;
using bn::core::StorageStatus;
using bn::epub::Bundle;
using bn::epub::BundleChapter;
using bn::epub::BundleFile;
using bn::epub::BundleStatus;
using bn::epub::BundleTable;
using bn::epub::BundleTotals;
using bn::epub::ChapterState;
using bn::test::FakeStorage;
using Bytes = std::vector<std::uint8_t>;

const char* const kPath = "/b";
// How many of a bundle's chapters are looked at: a file can say it has
// 65,535 of them in a few bytes.
constexpr std::size_t kLookedAt = 200;

std::uint32_t Get16(const Bytes& from, std::size_t at) {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}

std::uint32_t Get32(const Bytes& from, std::size_t at) { return Get16(from, at) | (Get16(from, at + 2) << 16U); }

void Put32(Bytes& to, std::size_t at, std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    to[at + byte] = static_cast<std::uint8_t>(value >> (8U * byte));
  }
}

// Puts right the checksums a bundle's header page and slots carry, going
// by where the format has them and not by the code that reads them.
void Mend(Bytes& file, bool header, bool slots) {
  if (file.size() < 4096) {
    return;
  }
  if (slots) {
    const std::uint64_t used = 24 + (std::uint64_t{Get16(file, 28)} * 88);
    for (std::uint64_t slot = 0; slot < 2; ++slot) {
      const std::uint64_t begin = Get32(file, 36) + (slot * Get32(file, 40));
      if (begin >= 4096 && begin + used <= file.size()) {
        const auto from = static_cast<std::size_t>(begin);
        const auto count = static_cast<std::size_t>(used - 4);
        Put32(file, from + count, bn::core::Crc32(std::span(file).subspan(from, count)));
      }
    }
  }
  if (header) {
    Put32(file, 4092, bn::core::Crc32(std::span(file).first(4092)));
  }
}

// Whether a slot carries the checksum of its own bytes, as the format has
// it: for a file with a whole header page.
bool SlotIsWhole(const Bytes& file, std::uint64_t slot) {
  const std::uint64_t used = 24 + (std::uint64_t{Get16(file, 28)} * 88);
  const std::uint64_t begin = Get32(file, 36) + (slot * Get32(file, 40));
  if (begin + used > file.size()) {
    return false;
  }
  const auto from = static_cast<std::size_t>(begin);
  const auto count = static_cast<std::size_t>(used - 4);
  return Get32(file, from + count) == bn::core::Crc32(std::span(file).subspan(from, count));
}

// Runs a piece of work to its end, holding each step to what a step may
// do. Returns how many steps it took.
template <typename Step>
std::size_t Stepped(FakeStorage& storage, const Step& step, bool mayWrite) {
  std::size_t steps = 0;
  for (bool more = true; more; ++steps) {
    const std::size_t reads = storage.reads.size();
    const int writes = storage.writes;
    const int syncs = storage.syncs;
    more = step();
    BN_FUZZ_CHECK(storage.reads.size() <= reads + 1, "a step read storage more than once");
    BN_FUZZ_CHECK(storage.reads.size() == reads || storage.reads.back().size <= 4096, "a step read more than a page");
    BN_FUZZ_CHECK(storage.writes <= writes + (mayWrite ? 1 : 0), "a step wrote more than it may");
    BN_FUZZ_CHECK(storage.writes == writes || storage.written.back().size <= 4096, "a step wrote more than a page");
    BN_FUZZ_CHECK(storage.syncs <= syncs + (mayWrite ? 1 : 0), "a step synced more than it may");
    BN_FUZZ_CHECK(steps < 1'000'000, "the work did not end");
  }
  return steps;
}

// What a chapter's record has to be, in a bundle that opened.
void CheckRecord(const Bundle& bundle, const BundleChapter& record) {
  BN_FUZZ_CHECK(record.state <= ChapterState::kRefused, "a chapter in a state there is none of");
  if (record.segmentBytes != 0) {
    BN_FUZZ_CHECK(record.segmentOffset >= bundle.DataBegin(), "a segment before the segments begin");
    BN_FUZZ_CHECK(record.segmentOffset % 512 == 0, "a segment that begins part way through a sector");
    BN_FUZZ_CHECK(std::uint64_t{record.segmentOffset} + record.segmentBytes <= bundle.DataEnd(),
                  "a segment past where the segments end");
    BN_FUZZ_CHECK(std::uint64_t{record.directoryOffset} + record.directoryBytes <= record.segmentBytes,
                  "a directory out of its segment");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  // The file: so long, of nothing, and then whatever the pieces put in it.
  Bytes given(std::size_t{input.Number16()} * 4);
  for (;;) {
    const std::size_t at = std::size_t{input.Number16()} * 4;
    const std::span<const std::uint8_t> piece = input.Take(input.Number16());
    if (piece.empty()) {
      break;
    }
    if (given.size() < at + piece.size()) {
      given.resize(at + piece.size());
    }
    std::copy(piece.begin(), piece.end(), given.begin() + static_cast<std::ptrdiff_t>(at));
  }
  Mend(given, (choices & 1U) != 0, (choices & 2U) != 0);

  FakeStorage storage;
  BN_FUZZ_CHECK(storage.Start() == StorageStatus::kOk, "the storage did not start");
  storage.Put(kPath, given);
  alignas(4) std::array<std::uint8_t, 4096> fileBuffer{};
  alignas(4) std::array<std::uint8_t, 4096> page{};
  BlockFile file(fileBuffer);
  BN_FUZZ_CHECK(file.Open(storage, kPath, OpenMode::kUpdate) == StorageStatus::kOk, "the file did not open");

  // A bundle given less than a page to work in opens nothing.
  const std::span<std::uint8_t> lessThanAPage = std::span(page).first(4095);
  Bundle cramped(lessThanAPage);
  BN_FUZZ_CHECK(cramped.Open(file) == BundleStatus::kMisused && !cramped.IsOpen(), "a bundle opened with no page");

  Bundle bundle(page);
  BN_FUZZ_CHECK(bundle.BeginOpen(file) == BundleStatus::kOk, "opening did not begin");
  const std::size_t steps = Stepped(storage, [&bundle] { return bundle.Step(); }, false);
  BN_FUZZ_CHECK(steps <= 4 + (2 * ((given.size() / 4096) + 1)), "opening took more steps than the file has pages for");
  const BundleStatus opened = bundle.Status();
  BN_FUZZ_CHECK(opened == BundleStatus::kOk || opened == BundleStatus::kNotBundle ||
                    opened == BundleStatus::kOtherVersion || opened == BundleStatus::kDamaged,
                "opening ended in a status it should not");
  BN_FUZZ_CHECK(bundle.IsOpen() == (opened == BundleStatus::kOk), "open, and not opened; or the other way");
  if (given.size() < 4096 || !std::equal(given.begin(), given.begin() + 4, "BNBK")) {
    BN_FUZZ_CHECK(opened == BundleStatus::kNotBundle, "a file that does not begin as a bundle was taken for one");
  }
  if (opened == BundleStatus::kOk) {
    BN_FUZZ_CHECK(Get32(given, 4092) == bn::core::Crc32(std::span(given).first(4092)),
                  "a bundle opened whose header page fails its checksum");
    BN_FUZZ_CHECK(SlotIsWhole(given, 0) || SlotIsWhole(given, 1),
                  "a bundle opened with no slot that passes its checksum");
  }
  BundleChapter record;
  if (opened != BundleStatus::kOk) {
    BN_FUZZ_CHECK(bundle.ReadChapter(0, record) == BundleStatus::kMisused, "a record was had of a file not open");
    BN_FUZZ_CHECK(bundle.BeginCommit(0, record) == BundleStatus::kMisused, "a file not open took a chapter");
    return 0;
  }

  // Where things are.
  const std::size_t documents = bundle.Book().documents;
  std::uint64_t end = 4096;
  for (std::size_t table = 0; table < bn::epub::kBundleTables; ++table) {
    const Bundle::Place place = bundle.Table(static_cast<BundleTable>(table));
    BN_FUZZ_CHECK(place.offset == end, "a table does not follow the one before");
    end += place.bytes;
  }
  const std::uint64_t slots = std::uint64_t{bn::epub::BundleSlotBytes(documents)} * 2;
  BN_FUZZ_CHECK(end + slots <= bundle.DataBegin(), "the tables and the slots run into the segments");
  BN_FUZZ_CHECK(bundle.DataBegin() <= bundle.DataEnd() && bundle.DataEnd() <= given.size(),
                "the segments do not end within the file");
  BN_FUZZ_CHECK(bundle.DataEnd() % 512 == 0, "the segments end part way through a sector");
  BN_FUZZ_CHECK(bundle.Totals().notImported <= documents, "more chapters not imported than there are");
  BN_FUZZ_CHECK(bundle.IsOf(Get32(given, 12), Get32(given, 16), Get32(given, 20)), "it is not of the EPUB it says");
  BN_FUZZ_CHECK(!bundle.IsOf(Get32(given, 12) + 1, Get32(given, 16), Get32(given, 20)), "it is of an EPUB of any size");

  // A table's first bytes and its last, and none past them.
  for (std::size_t table = 0; table < bn::epub::kBundleTables; ++table) {
    const Bundle::Place place = bundle.Table(static_cast<BundleTable>(table));
    std::array<std::uint8_t, 7> some{};
    const std::size_t count = std::min<std::size_t>(some.size(), place.bytes);
    const std::span<std::uint8_t> got = std::span(some).first(count);
    for (const std::uint32_t from : {std::uint32_t{0}, place.bytes - static_cast<std::uint32_t>(count)}) {
      BN_FUZZ_CHECK(bundle.ReadTable(static_cast<BundleTable>(table), from, got) == BundleStatus::kOk,
                    "some of a table could not be read");
      BN_FUZZ_CHECK(std::equal(got.begin(), got.end(), given.begin() + place.offset + from),
                    "what was read of a table is not what the file holds");
    }
    BN_FUZZ_CHECK(bundle.ReadTable(static_cast<BundleTable>(table), place.bytes, std::span(some).first(1)) ==
                      BundleStatus::kMisused,
                  "a byte past a table's end was read");
  }

  // The records.
  std::vector<BundleChapter> before;
  for (std::size_t chapter = 0; chapter < std::min(documents, kLookedAt); ++chapter) {
    BN_FUZZ_CHECK(bundle.ReadChapter(chapter, record) == BundleStatus::kOk, "a chapter's record could not be read");
    CheckRecord(bundle, record);
    before.push_back(record);
  }
  BN_FUZZ_CHECK(bundle.ReadChapter(documents, record) == BundleStatus::kMisused, "a record past the last was read");
  BundleFile entry;
  if (documents != 0) {
    BN_FUZZ_CHECK(bundle.ReadSpine(0, entry) == BundleStatus::kOk, "the first document's record could not be read");
    BN_FUZZ_CHECK(bundle.ReadSpine(documents - 1, entry) == BundleStatus::kOk, "nor the last's");
  }
  BN_FUZZ_CHECK(bundle.ReadSpine(documents, entry) == BundleStatus::kMisused, "a document past the last was read");
  const std::size_t pictures = bundle.Book().pictures;
  if (pictures != 0) {
    BN_FUZZ_CHECK(bundle.ReadPicture(pictures - 1, entry) == BundleStatus::kOk,
                  "the last picture's record could not be read");
  }
  BN_FUZZ_CHECK(bundle.ReadPicture(pictures, entry) == BundleStatus::kMisused, "a picture past the last was read");

  // The tables, against their checksums.
  for (std::size_t table = 0; table < bn::epub::kBundleTables; ++table) {
    const Bundle::Place place = bundle.Table(static_cast<BundleTable>(table));
    BN_FUZZ_CHECK(bundle.BeginCheck(static_cast<BundleTable>(table)) == BundleStatus::kOk, "a check did not begin");
    const std::size_t took = Stepped(storage, [&bundle] { return bundle.Step(); }, false);
    BN_FUZZ_CHECK(took <= (place.bytes / 4096) + 2, "a check took more steps than the table has pages for");
    const bool whole = bn::core::Crc32(std::span(given).subspan(place.offset, place.bytes)) == place.checksum;
    BN_FUZZ_CHECK(bundle.Status() == (whole ? BundleStatus::kOk : BundleStatus::kDamaged),
                  "a table was not found as it is");
    BN_FUZZ_CHECK(bundle.IsOpen(), "a check closed the bundle");
  }

  // A chapter committed.
  if (documents == 0) {
    return 0;
  }
  constexpr std::array<std::uint32_t, 4> kSegmentBytes = {0, 1, 512, 700};
  const std::uint32_t segmentBytes = kSegmentBytes[(choices >> 2U) & 3U];
  const std::size_t chapter = ((choices >> 4U) & 15U) % std::min(documents, kLookedAt);
  BundleChapter added;
  added.state = segmentBytes == 0 ? ChapterState::kRefused : ChapterState::kWhole;
  added.reason = 9;
  added.characters = 4321;
  added.classCharacters = {1, 2, 3, 4, 5, 6};
  const std::uint32_t at = bundle.DataEnd();
  if (segmentBytes != 0) {
    added.segmentOffset = at;
    added.segmentBytes = segmentBytes;
    added.directoryOffset = segmentBytes - 1;
    added.directoryBytes = 1;
    const Bytes segment(segmentBytes, 0xA5);
    BN_FUZZ_CHECK(file.Seek(at) && file.Write(segment), "the segment could not be written");
  }
  const std::uint32_t sequence = bundle.Sequence();
  const BundleStatus began = bundle.BeginCommit(chapter, added);
  if (began == BundleStatus::kTooBig) {
    BN_FUZZ_CHECK(sequence == 0xFFFFFFFFU, "a commit was too big, with sequence numbers to spare");
    return 0;
  }
  BN_FUZZ_CHECK(began == BundleStatus::kOk, "a commit did not begin");
  storage.written.clear();
  const std::size_t pages = bn::epub::BundleSlotBytes(documents) / 4096;
  const std::size_t took = Stepped(storage, [&bundle] { return bundle.Step(); }, true);
  BN_FUZZ_CHECK(took <= pages + 3, "a commit took more steps than the slot has pages for");
  BN_FUZZ_CHECK(bundle.Status() == BundleStatus::kOk, "a commit failed, on storage that did not");
  // What it wrote: past where the segments ended, and one slot, which was
  // not the slot in force. With the slot it wrote spoiled, the file is the
  // bundle it was before, every chapter as it was.
  const std::uint32_t slotsAt = Get32(given, 36);
  const std::uint32_t slotBytes = bn::epub::BundleSlotBytes(documents);
  std::size_t written = 2;
  for (const FakeStorage::ReadCall& write : storage.written) {
    if (write.offset >= at) {
      continue;
    }
    BN_FUZZ_CHECK(write.offset >= slotsAt, "a commit wrote over the header or the tables");
    const std::size_t slot = (write.offset - slotsAt) / slotBytes;
    BN_FUZZ_CHECK(written == 2 || written == slot, "a commit wrote to both slots");
    BN_FUZZ_CHECK(std::uint64_t{write.offset} + write.size <= slotsAt + (std::uint64_t{slot + 1} * slotBytes),
                  "a commit wrote over the end of a slot");
    written = slot;
  }
  BN_FUZZ_CHECK(written < 2, "a commit wrote no slot");
  {
    Bytes earlier = storage.Contents(kPath);
    earlier[slotsAt + (written * slotBytes)] ^= 0xFFU;
    storage.Put("/earlier", earlier);
    alignas(4) std::array<std::uint8_t, 4096> otherBuffer{};
    alignas(4) std::array<std::uint8_t, 4096> otherPage{};
    BlockFile other(otherBuffer);
    BN_FUZZ_CHECK(other.Open(storage, "/earlier", OpenMode::kRead) == StorageStatus::kOk, "the copy did not open");
    Bundle was(otherPage);
    BN_FUZZ_CHECK(was.Open(other) == BundleStatus::kOk, "a commit wrote over the slot in force");
    BN_FUZZ_CHECK(was.Sequence() == sequence && was.DataEnd() == at, "the slot left standing is not the one in force");
    for (std::size_t each = 0; each < before.size(); ++each) {
      BN_FUZZ_CHECK(was.ReadChapter(each, record) == BundleStatus::kOk && record == before[each],
                    "the slot left standing has a chapter otherwise than it was");
    }
  }
  const std::uint32_t newEnd = segmentBytes == 0 ? at : at + (((segmentBytes + 511) / 512) * 512);
  BN_FUZZ_CHECK(bundle.DataEnd() == newEnd, "the segments do not end where the chapter's last sector does");
  const BundleTotals said = bundle.Totals();

  // And found again, from the card.
  bundle.Close();
  BN_FUZZ_CHECK(file.Open(storage, kPath, OpenMode::kRead) == StorageStatus::kOk, "the file did not open again");
  BN_FUZZ_CHECK(bundle.Open(file) == BundleStatus::kOk, "the bundle did not open after a commit");
  BN_FUZZ_CHECK(bundle.Sequence() == sequence + 1, "not the next sequence number");
  BN_FUZZ_CHECK(bundle.DataEnd() == newEnd, "the segments end elsewhere once opened again");
  BN_FUZZ_CHECK(bundle.Totals() == said, "the chapters come to something else once opened again");
  for (std::size_t other = 0; other < before.size(); ++other) {
    BN_FUZZ_CHECK(bundle.ReadChapter(other, record) == BundleStatus::kOk, "a record could not be read after a commit");
    BN_FUZZ_CHECK(record == (other == chapter ? added : before[other]), "a record is not as it was, or as committed");
  }
  if (segmentBytes != 0) {
    const Bytes& now = storage.Contents(kPath);
    BN_FUZZ_CHECK(
        std::all_of(now.begin() + at, now.begin() + at + segmentBytes, [](std::uint8_t byte) { return byte == 0xA5; }),
        "the segment is not what was written");
    BN_FUZZ_CHECK(
        std::all_of(now.begin() + at + segmentBytes, now.begin() + newEnd, [](std::uint8_t byte) { return byte == 0; }),
        "the rest of the segment's last sector is not nothing");
  }
  return 0;
}
