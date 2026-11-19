// bn-jpeg: reads JPEG pictures with the reader the firmware reads them with,
// and says what it made of them: for one file, its header, or its rows
// as gray; for an EPUB, a line for each JPEG in it.
//
// A picture read whole is written as one line:
//
//   STATUS CRC WIDTH HEIGHT KIND REDUCTION ROWS ROOM PASSED SCANS JUNK
//
// where STATUS is how the reading ended ("ok", or what went wrong, with a
// hyphen for a space: "bad-data"), CRC the CRC-32 of the rows of gray that
// were given, one after another, in hex; WIDTH, HEIGHT and KIND what the
// header says ("-" for each if it was not read; KIND with a hyphen for a
// space, "progressive-4:2:0"); REDUCTION how many times smaller each way it
// was read (1, 2, 4 or 8); ROWS how many rows were given; ROOM how much
// memory the rows need at that reduction (what the reader would have to be
// given); PASSED how many marker segments were passed over, SCANS how many
// scans, and JUNK how many bytes between segments were not a marker. For
// an EPUB each line ends with the entry's name, and a last line adds them
// up.
//
// Given a new size, `read` reads the picture at the reduction the
// firmware would for that size, unless one is given, and puts the rows
// through the scaler as well: CRC and ROWS are then the new picture's, and
// the line ends with
//
//   scaled NEWWIDTH NEWHEIGHT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/block_file.h"
#include "core/byte_source.h"
#include "core/checksum.h"
#include "core/inflate.h"
#include "core/storage.h"
#include "draw/jpeg.h"
#include "draw/scale.h"
#include "epub/zip.h"
#include "support/one_file.h"

namespace {

using bn::core::StorageStatus;
using bn::draw::JpegReader;
using bn::draw::JpegReduction;
using bn::draw::JpegStatus;
using bn::draw::Scaler;
using bn::draw::ScaleStatus;
using bn::epub::ZipStatus;

// The most memory for rows this tool will give a reader: a picture that
// needs more is said to have no room.
constexpr std::uint64_t kMostRoom = 268435456;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s read FILE [--pieces BYTES] [--rows BYTES] [--reduce N] [--width W] [--height H]\n"
               "                    [--out PICTURE]\n"
               "       %s header FILE\n"
               "       %s book FILE [--rows BYTES] [--reduce N]\n"
               "\n"
               "Reads JPEG pictures the way the firmware does.\n"
               "\n"
               "  read    reads the picture in FILE to its end, and prints a line:\n"
               "              STATUS CRC WIDTH HEIGHT KIND REDUCTION ROWS ROOM PASSED SCANS JUNK\n"
               "          how the reading ended; the CRC-32 of the rows of gray it gave, one\n"
               "          after another; what the header says; how many times smaller it was\n"
               "          read; how many rows it gave; how much memory its rows need; how many\n"
               "          marker segments and scans it passed over; and how many bytes between\n"
               "          segments were not a marker\n"
               "\n"
               "  --pieces BYTES  give the reader the file this many bytes at a time (default:\n"
               "                  4096, as the card's driver does)\n"
               "  --rows BYTES    how much memory the reader has for rows (default: what the\n"
               "                  picture needs, if that is no more than 256 MB)\n"
               "  --reduce N      read it N times smaller each way: 1, 2, 4 or 8 (default: 1,\n"
               "                  or with a new size what the firmware would choose for it; a\n"
               "                  progressive picture is always read at 8)\n"
               "  --width W       make the picture W pixels wide, and\n"
               "  --height H      H high, as the firmware's scaler does: with one of them, the\n"
               "                  other keeps the picture's shape. The CRC and the rows are then\n"
               "                  the new picture's, and the line ends with `scaled W H`\n"
               "  --out PICTURE   write the rows it gave there, as a PGM file: a byte a pixel\n"
               "\n"
               "  header  reads no more of the picture in FILE than its header, with no memory\n"
               "          for rows, as a book's importer does, and prints a line:\n"
               "              STATUS WIDTH HEIGHT KIND BYTES\n"
               "          where BYTES is how much of the file that took\n"
               "\n"
               "  book    reads every JPEG in the EPUB (or ZIP) FILE, an entry at a time, as\n"
               "          `read` does (at its whole size, or N times smaller with --reduce),\n"
               "          and prints a line for each, with the entry's name at its end; then a\n"
               "          line that adds them up\n"
               "\n"
               "Ends with 0 if every picture was read to its end, 1 if one was not or a file\n"
               "cannot be read or written, and 2 if the command is not understood.\n",
               program, program, program);
}

// A name as one word.
std::string Word(std::string_view name) {
  std::string word(name);
  std::replace(word.begin(), word.end(), ' ', '-');
  return word;
}

std::string Escaped(std::string_view raw) {
  std::string out;
  for (const char ch : raw) {
    if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\\') {
      out += "\\\\";
    } else {
      out += ch;
    }
  }
  return out;
}

// Gives at most so many bytes at a call, and counts what it has given.
class Pieces final : public bn::core::ByteSource {
 public:
  Pieces(bn::core::ByteSource& source, std::size_t piece) : source_(source), piece_(std::max<std::size_t>(piece, 1)) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    const std::ptrdiff_t read = source_.Read(dst.first(std::min(dst.size(), piece_)));
    if (read > 0) {
      taken_ += static_cast<std::size_t>(read);
    }
    return read;
  }

  std::size_t Taken() const { return taken_; }

 private:
  bn::core::ByteSource& source_;
  std::size_t piece_;
  std::size_t taken_ = 0;
};

// Keeps the rows, and their checksum.
class Rows final : public bn::draw::RowSink {
 public:
  explicit Rows(bool keep) : keep_(keep) {}

  void Row(std::uint32_t /*y*/, std::span<const std::uint8_t> gray) noexcept override {
    crc_ = bn::core::Crc32(gray, crc_);
    if (keep_) {
      pixels_.insert(pixels_.end(), gray.begin(), gray.end());
    }
  }

  std::uint32_t Crc() const { return crc_; }
  const std::vector<std::uint8_t>& Pixels() const { return pixels_; }

 private:
  bool keep_;
  std::uint32_t crc_ = 0;
  std::vector<std::uint8_t> pixels_;
};

// A size asked for; 0 for a side that is not.
struct Size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  bool Asked() const { return width > 0 || height > 0; }
};

struct Result {
  JpegStatus status = JpegStatus::kOk;
  // The size the picture was scaled to, if it was.
  Size scaled;
  bool headerKnown = false;
  bn::draw::JpegHeader header;
  JpegReduction reduction = JpegReduction::kNone;
  std::uint32_t width = 0;  // As read, at the reduction.
  std::uint32_t crc = 0;
  std::uint32_t rows = 0;
  std::uint64_t room = 0;
  std::uint32_t passed = 0;
  std::uint32_t scans = 0;
  std::uint32_t junk = 0;
};

std::string Line(const Result& result) {
  std::array<char, 200> text{};
  if (!result.headerKnown) {
    std::snprintf(text.data(), text.size(), "%s %08x - - - - 0 0 %u %u %u",
                  Word(bn::draw::JpegStatusName(result.status)).c_str(), static_cast<unsigned>(result.crc),
                  static_cast<unsigned>(result.passed), static_cast<unsigned>(result.scans),
                  static_cast<unsigned>(result.junk));
  } else {
    std::snprintf(text.data(), text.size(), "%s %08x %u %u %s %u %u %llu %u %u %u",
                  Word(bn::draw::JpegStatusName(result.status)).c_str(), static_cast<unsigned>(result.crc),
                  static_cast<unsigned>(result.header.width), static_cast<unsigned>(result.header.height),
                  Word(bn::draw::JpegKindName(result.header)).c_str(), static_cast<unsigned>(result.reduction),
                  static_cast<unsigned>(result.rows), static_cast<unsigned long long>(result.room),
                  static_cast<unsigned>(result.passed), static_cast<unsigned>(result.scans),
                  static_cast<unsigned>(result.junk));
  }
  std::string line = text.data();
  if (result.scaled.Asked()) {
    line += " scaled " + std::to_string(result.scaled.width) + " " + std::to_string(result.scaled.height);
  }
  return line;
}

// The size a picture is made: what was asked, with a side that was not
// asked for keeping the picture's shape, rounded to the nearest pixel and
// at least one.
Size NewSize(const bn::draw::JpegHeader& header, Size asked) {
  Size size = asked;
  if (size.width == 0) {
    size.width = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, ((std::uint64_t{asked.height} * header.width * 2) + header.height) / (std::uint64_t{header.height} * 2)));
  } else if (size.height == 0) {
    size.height = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, ((std::uint64_t{asked.width} * header.height * 2) + header.width) / (std::uint64_t{header.width} * 2)));
  }
  return size;
}

// Reads the picture that `open` gives a source for, twice: once for its
// header, to know what its rows need, and once through. `open` is called
// once for each reading. `reduce` is 0 to have it chosen.
template <typename Open>
Result ReadPicture(Open open, std::size_t pieces, std::size_t rowLimit, unsigned reduce, Size asked, Rows& rows) {
  Result result;
  {
    JpegReader peek({});
    bn::core::ByteSource* source = open();
    if (source == nullptr) {
      result.status = JpegStatus::kSourceFailed;
      return result;
    }
    peek.Read(*source, nullptr);
    result.headerKnown = peek.HeaderKnown();
    result.header = peek.Header();
  }
  if (asked.Asked() && result.headerKnown && result.header.width > 0 && result.header.height > 0) {
    result.scaled = NewSize(result.header, asked);
  }
  JpegReduction reduction = static_cast<JpegReduction>(reduce == 0 ? 1 : reduce);
  if (reduce == 0 && result.scaled.Asked()) {
    reduction = JpegReader::ReductionFor(result.header, result.scaled.width, result.scaled.height);
  }
  result.reduction = JpegReader::ReductionTaken(result.header, reduction);
  result.room = result.headerKnown ? JpegReader::RowBytesFor(result.header, reduction) : 0;
  // As much as the picture needs, or as given; none if that is more than
  // kMostRoom, and the reader then says it has no room.
  const std::uint64_t wanted = rowLimit > 0 ? rowLimit : result.room;
  std::vector<std::uint8_t> rowMemory(wanted <= kMostRoom ? static_cast<std::size_t>(wanted) : 0);
  JpegReader reader(rowMemory);
  bn::core::ByteSource* source = open();
  if (source == nullptr) {
    result.status = JpegStatus::kSourceFailed;
    return result;
  }
  Pieces piecewise(*source, pieces);
  // Scaled, the reader's rows go through the scaler on their way to `rows`.
  const std::uint32_t scaledWidth = std::min(result.scaled.width, Scaler::kMaxSide);
  std::vector<std::uint32_t> sums(Scaler::SumsFor(scaledWidth));
  std::vector<std::uint8_t> scaledRow(scaledWidth);
  Scaler scaler(sums, scaledRow);
  bn::draw::RowSink* sink = &rows;
  if (result.scaled.Asked()) {
    const ScaleStatus begun =
        scaler.Begin(JpegReader::WidthAt(result.header, reduction), JpegReader::HeightAt(result.header, reduction),
                     result.scaled.width, result.scaled.height, rows);
    if (begun != ScaleStatus::kOk) {
      std::fprintf(stderr, "error: cannot scale to %ux%u: %s\n", static_cast<unsigned>(result.scaled.width),
                   static_cast<unsigned>(result.scaled.height), bn::draw::ScaleStatusName(begun));
      result.status = JpegStatus::kNoRoom;
      return result;
    }
    sink = &scaler;
  }
  reader.Begin(piecewise, sink, reduction);
  // The new rows a step of the reader makes ready are taken out after it,
  // the last step's too.
  bool more = true;
  while (more) {
    more = reader.Step();
    while (scaler.Step()) {
    }
  }
  result.status = reader.Status();
  result.headerKnown = reader.HeaderKnown();
  result.header = reader.Header();
  result.width = reader.Width();
  result.crc = rows.Crc();
  result.rows = result.scaled.Asked() ? scaler.RowsGiven() : reader.RowsGiven();
  result.passed = reader.SegmentsPassedOver();
  result.scans = reader.ScansPassedOver();
  result.junk = reader.JunkBytes();
  return result;
}

bool WritePgm(const std::string& path, std::size_t width, std::size_t height, const std::vector<std::uint8_t>& pixels) {
  std::FILE* out = std::fopen(path.c_str(), "wb");
  if (out == nullptr) {
    return false;
  }
  std::fprintf(out, "P5\n%zu %zu\n255\n", width, height);
  // With no rows there is nothing to write, and no memory to point at.
  const bool written = pixels.empty() || std::fwrite(pixels.data(), 1, pixels.size(), out) == pixels.size();
  return std::fclose(out) == 0 && written;
}

// A file of the computer's, open to be read as the card's are.
class FileOnDisk {
 public:
  explicit FileOnDisk(const std::string& path) : storage_(path), file_(buffer_) {}

  bn::core::ByteSource* Open() {
    file_.Close();
    if (!started_) {
      started_ = storage_.Start() == StorageStatus::kOk;
    }
    if (!started_ || file_.Open(storage_, "/jpeg", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
      return nullptr;
    }
    return &file_;
  }

 private:
  bn::tools::OneFile storage_;
  std::array<std::uint8_t, 4096> buffer_{};
  bn::core::BlockFile file_;
  bool started_ = false;
};

int Read(const std::string& path, std::size_t pieces, std::size_t rowLimit, unsigned reduce, Size asked,
         const std::string& out) {
  FileOnDisk file(path);
  if (file.Open() == nullptr) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Rows rows(!out.empty());
  const Result result = ReadPicture([&] { return file.Open(); }, pieces, rowLimit, reduce, asked, rows);
  std::printf("%s\n", Line(result).c_str());
  const std::uint32_t width = result.scaled.Asked() ? result.scaled.width : result.width;
  if (!out.empty() && !WritePgm(out, result.headerKnown ? width : 0, result.rows, rows.Pixels())) {
    std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
    return 1;
  }
  return result.status == JpegStatus::kOk ? 0 : 1;
}

int Header(const std::string& path) {
  FileOnDisk file(path);
  bn::core::ByteSource* source = file.Open();
  if (source == nullptr) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  Pieces counted(*source, 4096);
  JpegReader reader({});
  const JpegStatus status = reader.Read(counted, nullptr);
  if (reader.HeaderKnown()) {
    const bn::draw::JpegHeader& header = reader.Header();
    std::printf("%s %u %u %s %zu\n", Word(bn::draw::JpegStatusName(status)).c_str(),
                static_cast<unsigned>(header.width), static_cast<unsigned>(header.height),
                Word(bn::draw::JpegKindName(header)).c_str(), counted.Taken());
  } else {
    std::printf("%s - - - %zu\n", Word(bn::draw::JpegStatusName(status)).c_str(), counted.Taken());
  }
  return status == JpegStatus::kOk ? 0 : 1;
}

bool HasEnding(std::string_view name, std::string_view ending) {
  return name.size() > ending.size() &&
         std::equal(ending.begin(), ending.end(), name.end() - static_cast<std::ptrdiff_t>(ending.size()),
                    [](char a, char b) { return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b); });
}

bool HasJpegEnding(std::string_view name) { return HasEnding(name, ".jpg") || HasEnding(name, ".jpeg"); }

int Book(const std::string& path, std::size_t rowLimit, unsigned reduce) {
  bn::tools::OneFile storage(path);
  std::array<std::uint8_t, 4096> directoryBuffer{};
  std::array<std::uint8_t, 4096> entryBuffer{};
  bn::core::BlockFile directory{directoryBuffer};
  bn::core::BlockFile entries{entryBuffer};
  bn::epub::ZipArchive archive;
  if (storage.Start() != StorageStatus::kOk ||
      directory.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk ||
      entries.Open(storage, "/zip", bn::core::OpenMode::kRead) != StorageStatus::kOk) {
    std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
    return 1;
  }
  if (const ZipStatus status = archive.Open(directory); status != ZipStatus::kOk) {
    std::fprintf(stderr, "error: %s: %s\n", path.c_str(), bn::epub::ZipStatusName(status));
    return 1;
  }
  std::vector<std::uint8_t> zipWindow(bn::core::Inflater::kWindowBytes);
  bn::epub::ZipEntryReader entryReader{zipWindow};
  std::vector<char> name(65536);
  bn::epub::ZipArchive::Cursor cursor;
  std::uint32_t files = 0;
  std::uint32_t failed = 0;
  std::uint64_t bytes = 0;
  std::uint64_t pixels = 0;
  std::uint64_t mostRoom = 0;
  for (;;) {
    bn::epub::ZipEntry entry;
    const ZipStatus next = archive.Next(cursor, entry, name);
    if (next == ZipStatus::kEnd) {
      break;
    }
    if (next != ZipStatus::kOk && next != ZipStatus::kUnsupported) {
      std::fprintf(stderr, "error: %s: the directory: %s\n", path.c_str(), bn::epub::ZipStatusName(next));
      return 1;
    }
    const std::string_view entryName(name.data(), std::min<std::size_t>(entry.nameLength, name.size()));
    if (!HasJpegEnding(entryName)) {
      continue;
    }
    ++files;
    bool opened = next == ZipStatus::kOk;
    Rows rows(false);
    const Result result = ReadPicture(
        [&]() -> bn::core::ByteSource* {
          opened = opened && entryReader.Open(entries, entry) == ZipStatus::kOk;
          return opened ? &entryReader : nullptr;
        },
        4096, rowLimit, reduce == 0 ? 1 : reduce, Size{}, rows);
    // A picture that stopped where its entry could not be read further is
    // the ZIP file's fault, and said to be so.
    const bool entryFailed = !opened || entryReader.Status() != ZipStatus::kOk;
    if (result.status != JpegStatus::kOk || entryFailed) {
      ++failed;
    }
    bytes += entry.size;
    if (result.headerKnown) {
      pixels += std::uint64_t{result.header.width} * result.header.height;
      mostRoom = std::max(mostRoom, result.room);
    }
    std::printf("%s %s\n", (entryFailed ? std::string("unreadable") : Line(result)).c_str(),
                Escaped(entryName).c_str());
  }
  std::printf("jpeg_files=%u jpeg_failed=%u jpeg_bytes=%llu jpeg_pixels=%llu jpeg_most_room=%llu\n",
              static_cast<unsigned>(files), static_cast<unsigned>(failed), static_cast<unsigned long long>(bytes),
              static_cast<unsigned long long>(pixels), static_cast<unsigned long long>(mostRoom));
  return failed == 0 ? 0 : 1;
}

bool Number(const char* text, std::size_t& value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  value = static_cast<std::size_t>(parsed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  if (argc < 3) {
    PrintUsage(argv[0]);
    return 2;
  }
  const std::string path = argv[2];
  std::size_t pieces = 4096;
  std::size_t rowLimit = 0;
  std::size_t reduce = 0;
  std::size_t width = 0;
  std::size_t height = 0;
  std::string out;
  // Each option is followed by its value.
  for (int i = 3; i < argc; i += 2) {
    const std::string_view option = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    bool understood = false;
    if (value == nullptr) {
      understood = false;
    } else if (option == "--pieces" && command == "read") {
      understood = Number(value, pieces);
    } else if (option == "--rows" && command != "header") {
      understood = Number(value, rowLimit);
    } else if (option == "--reduce" && command != "header") {
      understood = Number(value, reduce) && (reduce == 1 || reduce == 2 || reduce == 4 || reduce == 8);
    } else if (option == "--width" && command == "read") {
      understood = Number(value, width) && width > 0 && width <= UINT32_MAX;
    } else if (option == "--height" && command == "read") {
      understood = Number(value, height) && height > 0 && height <= UINT32_MAX;
    } else if (option == "--out" && command == "read") {
      out = value;
      understood = true;
    }
    if (!understood) {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (command == "read") {
    return Read(path, pieces, rowLimit, static_cast<unsigned>(reduce),
                Size{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, out);
  }
  if (command == "header") {
    return Header(path);
  }
  if (command == "book") {
    return Book(path, rowLimit, static_cast<unsigned>(reduce));
  }
  PrintUsage(argv[0]);
  return 2;
}
