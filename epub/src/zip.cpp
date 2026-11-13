#include "epub/zip.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "core/checksum.h"
#include "core/endian.h"
#include "core/metrics.h"

namespace bn::epub {

namespace {

// The three kinds of record, each of which begins with its own four bytes,
// and how long each is before the parts whose length varies.
constexpr std::uint32_t kEndRecordSignature = 0x06054B50;
constexpr std::uint32_t kDirectorySignature = 0x02014B50;
constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::size_t kEndRecordBytes = 22;
constexpr std::size_t kDirectoryRecordBytes = 46;
constexpr std::size_t kLocalHeaderBytes = 30;

// The end record may be followed by a comment of up to this many bytes.
constexpr std::uint32_t kMaxCommentBytes = 65535;

// What a field holds when its real value is too big for it and is kept
// elsewhere, in the ZIP64 way.
constexpr std::uint32_t kZip64Count = 0xFFFF;
constexpr std::uint32_t kZip64Value = 0xFFFFFFFF;

// The flag bits that say an entry's data is encrypted, or that the
// directory itself has been.
constexpr std::uint32_t kEncryptedFlags = 0x0001U | 0x0040U | 0x2000U;

core::Counter zipFinds{"zip_finds"};      // Searches for an entry by name.
core::Counter zipRecords{"zip_records"};  // Directory records read from the file.

std::uint32_t U16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe16(std::span<const std::uint8_t, 2>(bytes.subspan(at, 2)));
}

std::uint32_t U32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe32(std::span<const std::uint8_t, 4>(bytes.subspan(at, 4)));
}

// Whether `record`, which is `offset` bytes into a file of `fileSize`,
// can be the end record: it begins as one does, and the comment it says
// comes after it fits in the file.
bool IsEndRecord(std::span<const std::uint8_t> record, std::uint64_t offset, std::uint32_t fileSize) noexcept {
  return U32(record, 0) == kEndRecordSignature && offset + kEndRecordBytes + U16(record, 20) <= fileSize;
}

// Finds the end record, copies it to `record` and says where it was.
ZipStatus FindEndRecord(core::BlockFile& file, std::span<std::uint8_t, kEndRecordBytes> record,
                        std::uint32_t& offset) noexcept {
  const std::uint32_t fileSize = file.Size();
  if (fileSize < kEndRecordBytes) {
    return ZipStatus::kNotZip;
  }
  // Nearly always it is the last thing in the file.
  offset = fileSize - static_cast<std::uint32_t>(kEndRecordBytes);
  if (!file.ReadAt(offset, record)) {
    return ZipStatus::kStorageFailed;
  }
  if (IsEndRecord(record, offset, fileSize)) {
    return ZipStatus::kOk;
  }

  // Otherwise it has a comment after it, and is somewhere in as much of the
  // end of the file as the longest comment would put it. The last place in
  // there that could be it, is it: that is what looking backwards from the
  // end of the file, as other readers do, finds first.
  const std::uint32_t reach = static_cast<std::uint32_t>(kEndRecordBytes) + kMaxCommentBytes;
  std::uint32_t position = fileSize > reach ? fileSize - reach : 0;  // Of the first byte in `chunk`.
  if (!file.Seek(position)) {
    return ZipStatus::kStorageFailed;
  }
  std::array<std::uint8_t, 256> chunk{};
  std::size_t held = 0;  // Bytes at the start of `chunk` kept from the last time round.
  bool found = false;
  for (;;) {
    const std::ptrdiff_t read = file.Read(std::span(chunk).subspan(held));
    if (read < 0) {
      return ZipStatus::kStorageFailed;
    }
    if (read == 0) {
      break;
    }
    const std::size_t have = held + static_cast<std::size_t>(read);
    std::size_t at = 0;
    for (; at + kEndRecordBytes <= have; ++at) {
      const std::span<const std::uint8_t> candidate = std::span<const std::uint8_t>(chunk).subspan(at, kEndRecordBytes);
      if (IsEndRecord(candidate, std::uint64_t{position} + at, fileSize)) {
        std::copy(candidate.begin(), candidate.end(), record.begin());
        offset = position + static_cast<std::uint32_t>(at);
        found = true;
      }
    }
    // The bytes too few to be looked at yet go to the front, to be joined
    // by the next ones.
    std::copy(chunk.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin() + static_cast<std::ptrdiff_t>(have),
              chunk.begin());
    held = have - at;
    position += static_cast<std::uint32_t>(at);
  }
  return found ? ZipStatus::kOk : ZipStatus::kNotZip;
}

ZipStatus FromInflater(core::Inflater::Status status) noexcept {
  switch (status) {
    case core::Inflater::Status::kSourceFailed:
      return ZipStatus::kStorageFailed;
    case core::Inflater::Status::kNoWindow:
      return ZipStatus::kNoWindow;
    default:
      return ZipStatus::kDamaged;
  }
}

}  // namespace

// What the header says of its size.
static_assert(sizeof(ZipEntryReader) < 3072);

const char* ZipStatusName(ZipStatus status) noexcept {
  switch (status) {
    case ZipStatus::kOk:
      return "ok";
    case ZipStatus::kEnd:
      return "end";
    case ZipStatus::kNotFound:
      return "not found";
    case ZipStatus::kNotOpen:
      return "not open";
    case ZipStatus::kNotZip:
      return "not a zip file";
    case ZipStatus::kDamaged:
      return "damaged";
    case ZipStatus::kBadChecksum:
      return "bad checksum";
    case ZipStatus::kUnsupported:
      return "unsupported";
    case ZipStatus::kStorageFailed:
      return "storage failed";
    case ZipStatus::kNoWindow:
      return "no window";
  }
  return "unknown";
}

// ---- The directory ----

// One record of the directory.
struct ZipArchive::Record {
  ZipEntry entry;
  std::uint32_t next = 0;   // Where the record after it starts.
  bool needsZip64 = false;  // Its sizes or its place are not in it, but kept the ZIP64 way.
};

ZipStatus ZipArchive::Open(core::BlockFile& file) noexcept {
  Close();
  if (!file.IsOpen()) {
    return ZipStatus::kNotOpen;
  }
  std::array<std::uint8_t, kEndRecordBytes> record{};
  std::uint32_t recordOffset = 0;
  if (const ZipStatus status = FindEndRecord(file, record, recordOffset); status != ZipStatus::kOk) {
    return status;
  }
  const std::uint32_t disk = U16(record, 4);
  const std::uint32_t directoryDisk = U16(record, 6);
  const std::uint32_t entriesHere = U16(record, 8);
  const std::uint32_t entries = U16(record, 10);
  const std::uint32_t directorySize = U32(record, 12);
  const std::uint32_t directoryOffset = U32(record, 16);
  if (entries == kZip64Count || entriesHere == kZip64Count || directorySize == kZip64Value ||
      directoryOffset == kZip64Value) {
    return ZipStatus::kUnsupported;
  }
  // A file that is one of several parts.
  if (disk != 0 || directoryDisk != 0 || entriesHere != entries) {
    return ZipStatus::kUnsupported;
  }
  // The directory lies before the end record, and has room for as many
  // records as there are said to be.
  if (std::uint64_t{directoryOffset} + directorySize > recordOffset ||
      std::uint64_t{entries} * kDirectoryRecordBytes > directorySize) {
    return ZipStatus::kDamaged;
  }
  file_ = &file;
  directoryOffset_ = directoryOffset;
  directoryEnd_ = directoryOffset + directorySize;
  entryCount_ = entries;
  searchOffset_ = directoryOffset;
  searchIndex_ = 0;
  return ZipStatus::kOk;
}

void ZipArchive::Close() noexcept {
  file_ = nullptr;
  directoryOffset_ = 0;
  directoryEnd_ = 0;
  entryCount_ = 0;
  searchOffset_ = 0;
  searchIndex_ = 0;
}

ZipStatus ZipArchive::ReadRecord(std::uint32_t offset, Record& record) noexcept {
  if (offset < directoryOffset_ || std::uint64_t{offset} + kDirectoryRecordBytes > directoryEnd_) {
    return ZipStatus::kDamaged;
  }
  std::array<std::uint8_t, kDirectoryRecordBytes> bytes{};
  if (!file_->ReadAt(offset, bytes)) {
    return ZipStatus::kStorageFailed;
  }
  zipRecords.Add();
  if (U32(bytes, 0) != kDirectorySignature) {
    return ZipStatus::kDamaged;
  }
  ZipEntry& entry = record.entry;
  entry.flags = static_cast<std::uint16_t>(U16(bytes, 8));
  entry.method = static_cast<std::uint16_t>(U16(bytes, 10));
  entry.crc32 = U32(bytes, 16);
  entry.compressedSize = U32(bytes, 20);
  entry.size = U32(bytes, 24);
  entry.nameLength = static_cast<std::uint16_t>(U16(bytes, 28));
  entry.headerOffset = U32(bytes, 42);
  // After the name come two more things of varying length, which are of no
  // interest here.
  const std::uint64_t next =
      std::uint64_t{offset} + kDirectoryRecordBytes + entry.nameLength + U16(bytes, 30) + U16(bytes, 32);
  if (next > directoryEnd_) {
    return ZipStatus::kDamaged;
  }
  record.next = static_cast<std::uint32_t>(next);
  record.needsZip64 =
      entry.compressedSize == kZip64Value || entry.size == kZip64Value || entry.headerOffset == kZip64Value;
  return ZipStatus::kOk;
}

ZipStatus ZipArchive::NameIs(std::uint32_t offset, std::string_view name) noexcept {
  std::array<std::uint8_t, 64> piece{};
  std::size_t done = 0;
  while (done < name.size()) {
    const std::size_t count = std::min(piece.size(), name.size() - done);
    const auto at = static_cast<std::uint32_t>(offset + kDirectoryRecordBytes + done);
    if (!file_->ReadAt(at, std::span(piece).first(count))) {
      return ZipStatus::kStorageFailed;
    }
    if (std::memcmp(piece.data(), name.data() + done, count) != 0) {
      return ZipStatus::kNotFound;
    }
    done += count;
  }
  return ZipStatus::kOk;
}

ZipStatus ZipArchive::Find(std::string_view name, ZipEntry& entry) noexcept {
  if (!IsOpen()) {
    return ZipStatus::kNotOpen;
  }
  zipFinds.Add();
  std::uint32_t offset = searchOffset_;
  std::uint32_t index = searchIndex_;
  for (std::uint32_t tried = 0; tried < entryCount_; ++tried) {
    if (index >= entryCount_) {
      offset = directoryOffset_;
      index = 0;
    }
    Record record;
    if (const ZipStatus status = ReadRecord(offset, record); status != ZipStatus::kOk) {
      return status;
    }
    if (record.entry.nameLength == name.size()) {
      const ZipStatus same = NameIs(offset, name);
      if (same == ZipStatus::kOk) {
        searchOffset_ = record.next;
        searchIndex_ = index + 1;
        if (record.needsZip64) {
          return ZipStatus::kUnsupported;
        }
        entry = record.entry;
        return ZipStatus::kOk;
      }
      if (same != ZipStatus::kNotFound) {
        return same;
      }
    }
    offset = record.next;
    ++index;
  }
  return ZipStatus::kNotFound;
}

ZipStatus ZipArchive::Next(Cursor& cursor, ZipEntry& entry, std::span<char> name) noexcept {
  if (!IsOpen()) {
    return ZipStatus::kNotOpen;
  }
  if (cursor.index >= entryCount_) {
    return ZipStatus::kEnd;
  }
  const std::uint32_t offset = cursor.index == 0 ? directoryOffset_ : cursor.offset;
  Record record;
  if (const ZipStatus status = ReadRecord(offset, record); status != ZipStatus::kOk) {
    return status;
  }
  const std::size_t count = std::min<std::size_t>(name.size(), record.entry.nameLength);
  if (count > 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, read as the bytes they are.
    const std::span<std::uint8_t> bytes(reinterpret_cast<std::uint8_t*>(name.data()), count);
    if (!file_->ReadAt(offset + static_cast<std::uint32_t>(kDirectoryRecordBytes), bytes)) {
      return ZipStatus::kStorageFailed;
    }
  }
  cursor.offset = record.next;
  ++cursor.index;
  entry = record.entry;
  return record.needsZip64 ? ZipStatus::kUnsupported : ZipStatus::kOk;
}

// ---- One entry ----

void ZipEntryReader::Raw::Start(core::BlockFile& file, std::uint32_t offset, std::uint32_t length) noexcept {
  file_ = &file;
  position_ = offset;
  left_ = length;
}

std::ptrdiff_t ZipEntryReader::Raw::Read(std::span<std::uint8_t> dst) noexcept {
  const std::size_t count = std::min<std::size_t>(dst.size(), left_);
  if (count == 0) {
    return 0;
  }
  // The file may have been used for something else since the last time.
  if (file_ == nullptr || !file_->Seek(position_)) {
    return kError;
  }
  const std::ptrdiff_t read = file_->Read(dst.first(count));
  if (read <= 0) {
    return kError;  // Nothing, where the file was seen to have something: it is no longer what it was.
  }
  position_ += static_cast<std::uint32_t>(read);
  left_ -= static_cast<std::uint32_t>(read);
  return read;
}

ZipEntryReader::ZipEntryReader(std::span<std::uint8_t> window) noexcept
    : inflater_(raw_, window), hasWindow_(window.size() >= core::Inflater::kWindowBytes) {}

ZipStatus ZipEntryReader::Open(core::BlockFile& file, const ZipEntry& entry) noexcept {
  finished_ = false;
  remaining_ = 0;
  status_ = ZipStatus::kNotOpen;
  // Whatever stops the entry being read is also what Status() says after.
  const auto refuse = [this](ZipStatus status) noexcept {
    status_ = status;
    return status;
  };
  if (!file.IsOpen()) {
    return ZipStatus::kNotOpen;
  }
  if ((std::uint32_t{entry.flags} & kEncryptedFlags) != 0 ||
      (entry.method != kZipStored && entry.method != kZipDeflated)) {
    return refuse(ZipStatus::kUnsupported);
  }
  if (entry.method == kZipStored && entry.compressedSize != entry.size) {
    return refuse(ZipStatus::kDamaged);
  }
  if (entry.method == kZipDeflated && !hasWindow_) {
    return refuse(ZipStatus::kNoWindow);
  }

  // The entry starts with a header of its own, which says how long it is
  // itself; the data comes after it.
  const std::uint32_t fileSize = file.Size();
  if (std::uint64_t{entry.headerOffset} + kLocalHeaderBytes > fileSize) {
    return refuse(ZipStatus::kDamaged);
  }
  std::array<std::uint8_t, kLocalHeaderBytes> header{};
  if (!file.ReadAt(entry.headerOffset, header)) {
    return refuse(ZipStatus::kStorageFailed);
  }
  if (U32(header, 0) != kLocalSignature) {
    return refuse(ZipStatus::kDamaged);
  }
  const std::uint64_t dataOffset =
      std::uint64_t{entry.headerOffset} + kLocalHeaderBytes + U16(header, 26) + U16(header, 28);
  if (dataOffset + entry.compressedSize > fileSize) {
    return refuse(ZipStatus::kDamaged);
  }

  raw_.Start(file, static_cast<std::uint32_t>(dataOffset), entry.compressedSize);
  deflated_ = entry.method == kZipDeflated;
  if (deflated_) {
    inflater_.Reset(raw_);
  }
  remaining_ = entry.size;
  crc_ = 0;
  expectedCrc_ = entry.crc32;
  status_ = ZipStatus::kOk;
  return ZipStatus::kOk;
}

std::ptrdiff_t ZipEntryReader::Fail(ZipStatus status) noexcept {
  if (status_ == ZipStatus::kOk) {
    status_ = status;
  }
  return kError;
}

// Called once every byte the directory promised has been given out: checks
// that the data ends there too, and that the bytes were the right ones.
void ZipEntryReader::Finish() noexcept {
  finished_ = true;
  if (deflated_) {
    std::array<std::uint8_t, 1> more{};
    const std::ptrdiff_t read = inflater_.Read(more);
    if (read > 0) {
      status_ = ZipStatus::kDamaged;  // It holds more than the directory says.
      return;
    }
    if (read < 0) {
      status_ = FromInflater(inflater_.CurrentStatus());
      return;
    }
  }
  if (crc_ != expectedCrc_) {
    status_ = ZipStatus::kBadChecksum;
  }
}

std::ptrdiff_t ZipEntryReader::Read(std::span<std::uint8_t> dst) noexcept {
  if (status_ != ZipStatus::kOk) {
    return kError;
  }
  if (!finished_ && remaining_ == 0) {
    Finish();  // An entry with nothing in it.
    if (status_ != ZipStatus::kOk) {
      return kError;
    }
  }
  if (finished_ || dst.empty()) {
    return 0;
  }

  const std::span<std::uint8_t> wanted = dst.first(std::min<std::size_t>(dst.size(), remaining_));
  const std::ptrdiff_t read = deflated_ ? inflater_.Read(wanted) : raw_.Read(wanted);
  if (read < 0) {
    return Fail(deflated_ ? FromInflater(inflater_.CurrentStatus()) : ZipStatus::kStorageFailed);
  }
  if (read == 0) {
    return Fail(ZipStatus::kDamaged);  // It holds less than the directory says.
  }
  const std::span<const std::uint8_t> got = wanted.first(static_cast<std::size_t>(read));
  crc_ = core::Crc32(got, crc_);
  remaining_ -= static_cast<std::uint32_t>(read);
  if (remaining_ == 0) {
    // Checked now, so that Status() is right as soon as the last byte has
    // been handed over. If the check fails, the next call says so.
    Finish();
  }
  return read;
}

}  // namespace bn::epub
