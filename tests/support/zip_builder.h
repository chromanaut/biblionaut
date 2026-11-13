#pragma once

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Making ZIP files in memory for tests: good ones, with the compressing and
// the checksums done by zlib, and a note of where each part went so that a
// test can then spoil it.

namespace bn::test {

// Where the fields of the three kinds of ZIP record are, counted from the
// start of the record.
namespace zip_field {

// A directory record.
inline constexpr std::size_t kRecordFlags = 8;
inline constexpr std::size_t kRecordMethod = 10;
inline constexpr std::size_t kRecordCrc = 16;
inline constexpr std::size_t kRecordCompressedSize = 20;
inline constexpr std::size_t kRecordSize = 24;
inline constexpr std::size_t kRecordNameLength = 28;
inline constexpr std::size_t kRecordExtraLength = 30;
inline constexpr std::size_t kRecordCommentLength = 32;
inline constexpr std::size_t kRecordHeaderOffset = 42;
inline constexpr std::size_t kRecordName = 46;

// The end record.
inline constexpr std::size_t kEndDisk = 4;
inline constexpr std::size_t kEndDirectoryDisk = 6;
inline constexpr std::size_t kEndEntriesHere = 8;
inline constexpr std::size_t kEndEntries = 10;
inline constexpr std::size_t kEndDirectorySize = 12;
inline constexpr std::size_t kEndDirectoryOffset = 16;
inline constexpr std::size_t kEndCommentLength = 20;

// The header in front of an entry's data.
inline constexpr std::size_t kHeaderSignature = 0;
inline constexpr std::size_t kHeaderNameLength = 26;
inline constexpr std::size_t kHeaderExtraLength = 28;

}  // namespace zip_field

// Writes a 16-bit or 32-bit number into `bytes` the way ZIP keeps them.
inline void Put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  bytes.at(at) = static_cast<std::uint8_t>(value);
  bytes.at(at + 1) = static_cast<std::uint8_t>(value >> 8U);
}

inline void Put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  Put16(bytes, at, value & 0xFFFFU);
  Put16(bytes, at + 2, value >> 16U);
}

// How one entry is to be written.
struct ZipEntryOptions {
  bool deflate = false;  // Compressed; otherwise kept as it is.
  // The sizes and checksum left out of the entry's own header and put
  // after its data, as a writer does that cannot go back.
  bool descriptor = false;
  // The three below are given an initializer they do not need, because
  // without one the compiler warns of every `{.deflate = true}` that
  // leaves them out.
  // NOLINTBEGIN(readability-redundant-member-init)
  std::vector<std::uint8_t> headerExtra{};  // The "extra field" in the entry's own header,
  std::vector<std::uint8_t> recordExtra{};  // and the one in its directory record.
  std::string comment{};                    // The entry's comment, in its directory record.
  // NOLINTEND(readability-redundant-member-init)
  std::uint32_t flags = 0;  // Flag bits to set, in both.
};

// Where an entry's parts are in the file.
struct ZipPlaced {
  std::size_t header = 0;    // Its own header.
  std::size_t data = 0;      // Its data,
  std::size_t dataSize = 0;  // and how much of that there is.
  std::size_t record = 0;    // Its directory record. Known once Finish() has run.
};

class ZipBuilder {
 public:
  using Bytes = std::vector<std::uint8_t>;
  using Options = ZipEntryOptions;
  using Placed = ZipPlaced;

  ZipBuilder& Add(const std::string& name, const Bytes& contents, const Options& options = {}) {
    Item item;
    item.name = name;
    item.options = options;
    item.crc = static_cast<std::uint32_t>(crc32(0, contents.data(), static_cast<uInt>(contents.size())));
    item.size = static_cast<std::uint32_t>(contents.size());
    const Bytes data = options.deflate ? Deflated(contents) : contents;
    item.compressedSize = static_cast<std::uint32_t>(data.size());
    item.flags = options.flags | (options.descriptor ? 0x0008U : 0U);
    item.placed.header = bytes_.size();

    Append32(0x04034B50);
    Append16(20);  // The version of ZIP needed to read it.
    Append16(item.flags);
    Append16(options.deflate ? 8 : 0);
    Append16(0);       // Time,
    Append16(0x0021);  // and date: 1 January 1980.
    Append32(options.descriptor ? 0 : item.crc);
    Append32(options.descriptor ? 0 : item.compressedSize);
    Append32(options.descriptor ? 0 : item.size);
    Append16(static_cast<std::uint32_t>(name.size()));
    Append16(static_cast<std::uint32_t>(options.headerExtra.size()));
    bytes_.insert(bytes_.end(), name.begin(), name.end());
    Append(options.headerExtra);
    item.placed.data = bytes_.size();
    item.placed.dataSize = data.size();
    Append(data);
    if (options.descriptor) {
      Append32(0x08074B50);
      Append32(item.crc);
      Append32(item.compressedSize);
      Append32(item.size);
    }
    items_.push_back(item);
    return *this;
  }

  // Bytes that belong to no entry.
  ZipBuilder& Loose(const Bytes& bytes) {
    Append(bytes);
    return *this;
  }

  // Writes the directory and the end record, with `comment` after it, and
  // gives the whole file.
  Bytes Finish(const Bytes& comment = {}) {
    directory_ = bytes_.size();
    for (Item& item : items_) {
      item.placed.record = bytes_.size();
      Append32(0x02014B50);
      Append16(20);  // The version that made it,
      Append16(20);  // and the one needed to read it.
      Append16(item.flags);
      Append16(item.options.deflate ? 8 : 0);
      Append16(0);
      Append16(0x0021);
      Append32(item.crc);
      Append32(item.compressedSize);
      Append32(item.size);
      Append16(static_cast<std::uint32_t>(item.name.size()));
      Append16(static_cast<std::uint32_t>(item.options.recordExtra.size()));
      Append16(static_cast<std::uint32_t>(item.options.comment.size()));
      Append16(0);  // The part of a file in several parts that it is in.
      Append16(0);  // Attributes,
      Append32(0);  // and more attributes.
      Append32(static_cast<std::uint32_t>(item.placed.header));
      bytes_.insert(bytes_.end(), item.name.begin(), item.name.end());
      Append(item.options.recordExtra);
      bytes_.insert(bytes_.end(), item.options.comment.begin(), item.options.comment.end());
    }
    const std::size_t directorySize = bytes_.size() - directory_;
    endRecord_ = bytes_.size();
    Append32(0x06054B50);
    Append16(0);
    Append16(0);
    Append16(static_cast<std::uint32_t>(items_.size()));
    Append16(static_cast<std::uint32_t>(items_.size()));
    Append32(static_cast<std::uint32_t>(directorySize));
    Append32(static_cast<std::uint32_t>(directory_));
    Append16(static_cast<std::uint32_t>(comment.size()));
    Append(comment);
    return bytes_;
  }

  // Where things went. The directory and the end record are known once
  // Finish() has run.
  const Placed& At(std::size_t index) const { return items_.at(index).placed; }
  std::size_t Directory() const { return directory_; }
  std::size_t EndRecord() const { return endRecord_; }

  // `contents` compressed with DEFLATE, with nothing around it.
  static Bytes Deflated(const Bytes& contents) {
    z_stream stream = {};
    Bytes out;
    if (deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      return out;
    }
    out.resize(deflateBound(&stream, static_cast<uLong>(contents.size())));
    Bytes input = contents;  // zlib's interface is not const-correct about its input.
    stream.next_in = input.data();
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    out.resize(result == Z_STREAM_END ? stream.total_out : 0);
    deflateEnd(&stream);
    return out;
  }

 private:
  struct Item {
    std::string name;
    Options options;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t flags = 0;
    Placed placed;
  };

  void Append16(std::uint32_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value));
    bytes_.push_back(static_cast<std::uint8_t>(value >> 8U));
  }
  void Append32(std::uint32_t value) {
    Append16(value & 0xFFFFU);
    Append16(value >> 16U);
  }
  void Append(const Bytes& bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }

  Bytes bytes_;
  std::vector<Item> items_;
  std::size_t directory_ = 0;
  std::size_t endRecord_ = 0;
};

}  // namespace bn::test
