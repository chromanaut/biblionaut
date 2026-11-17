#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/byte_source.h"
#include "core/inflate.h"
#include "draw/row_sink.h"

// Reading a PNG picture, as the W3C's specification of PNG has it.
//
// A picture is read from a ByteSource a piece at a time and never held
// whole: first its header, which says how large it is and what kind of
// picture, and then, if they are wanted, its rows, top first, each given
// to a RowSink as soon as it is whole. All that is kept of the picture
// is the row being read and the one above it, which PNG's filters refer
// back to.
//
// What is read:
//
//   - Every kind of picture PNG has: gray at 1, 2, 4, 8 and 16 bits a
//     sample; colour (RGB), gray with alpha, and colour with alpha (RGBA),
//     at 8 and 16; colours from a palette, at 1, 2, 4 and 8.
//   - Transparency, by an alpha sample, by a palette's alphas, or by a
//     single gray or colour that stands for "none" (`tRNS`).
//
// What is given out is 8-bit gray, a byte a pixel, as the picture looks on
// white paper: a colour as the gray it looks (GrayOf() in draw/gray.h),
// and a pixel that is not wholly solid as that much of its gray over white
// (OverWhite()). A sample of 16 bits is taken by its high byte; one of
// fewer than 8 bits is stretched to the whole range, so that its largest
// value is white. A palette index past the end of the palette is black.
//
// What is not read:
//
//   - Interlaced pictures. Their header is read, and their rows refused
//     (kInterlaced): an interlaced picture is given in seven passes over
//     the whole of it, and none of its rows is whole until the last, so it
//     cannot be read a row at a time.
//   - Anything about colour itself (`gAMA`, `cHRM`, `sRGB`, `iCCP`,
//     `sBIT`): the picture becomes gray, for paper.
//   - The colour a picture says to put behind it (`bKGD`): it is laid on
//     the page.
//   - Animation. An animated picture is read as its first picture, which
//     is what its `IDAT` chunks hold.
//   - Text, and every other chunk that a reader may pass over: passed
//     over.
//
// The file is not trusted. Whatever its bytes are, reading ends in a
// status. Each chunk's checksum (its CRC) is checked as the chunk is read:
// a chunk whose checksum is wrong ends the reading if the picture needs it
// (`IHDR`, `PLTE`, `IDAT`), and is passed over if not. A chunk's checksum
// comes after its bytes, so the rows made from a chunk of data have been
// given by the time its checksum is found wrong. The checksum at the end
// of the compressed data (zlib's Adler-32) is not checked: by the time it
// comes the inflater has already read it, as it reads ahead, and every
// byte of it has been through its chunk's CRC.
//
// Memory. The inflater's window, 32 KB (kWindowBytes), and room for two
// rows (RowBytesFor()) are the caller's, and given when the reader is
// made. Reading only the header needs neither, and either may be empty
// for that. The object itself is 3,048 bytes on the device: most of it the
// inflater, and the palette as grays.
//
// One call's work. Step() does one of these and returns: it reads PNG's
// signature; or the start of a chunk; or up to kChunkBytes of a chunk,
// and if that is the last of it, the chunk's checksum; or the two bytes
// that begin the compressed data; or it asks the inflater once for up to
// kRowBytesPerStep bytes of a row, and if that completes the row, undoes
// the row's filter, turns it to gray, and gives it to the sink. The
// inflater's own bound (core::Inflater::MostInputFor()) holds the bytes of
// compressed data that call takes. Between them lie the starts and ends of
// chunks: a picture's data may be cut into chunks of any length, down to
// one byte, and a few chunks with nothing in them are passed over between
// two that have something (kMaxEmptyChunksInARow; more is refused). So the
// most a call takes from its source, whatever the file, is
// MostInputPerStep(): 284,196 bytes, for a file whose data is in chunks of
// one byte each with empty ones between. Pictures as encoders write them
// have their data in chunks of 8 KB or more, and a call takes about what
// the inflater does.
//
// Used by one task.

namespace bn::draw {

// How reading a picture has gone.
enum class PngStatus : std::uint8_t {
  kOk = 0,        // Well so far, or to the end.
  kNotPng,        // It does not begin with PNG's signature.
  kBadHeader,     // Its first chunk is not a header, or not one PNG allows.
  kBadChecksum,   // A chunk the picture needs has the wrong checksum.
  kBadChunk,      // A chunk is not as PNG allows: longer than a chunk may be, with a name that is not four
                  // letters, a second header or palette, or a palette of the wrong length.
  kUnknownChunk,  // A chunk that a reader has to understand to read the picture, and this one does not.
  kNoPalette,     // A picture of palette colours has no palette before its data.
  kBadData,       // Its compressed data is not what PNG's is, or a row has a filter PNG does not have.
  kTruncated,     // It ended before its last row. The rows before that were given.
  kInterlaced,    // Its rows are interlaced. The header was read; no rows are given.
  kNoRoom,        // The window or the room for rows it was given is too small for this picture.
  kSourceFailed,  // The source reported an error.
};

// The name of a status, for diagnostics: "ok", "not png", ...
const char* PngStatusName(PngStatus status) noexcept;

// What kind of picture it is, by the number PNG gives it.
enum class PngColour : std::uint8_t {
  kGray = 0,
  kRgb = 2,
  kPalette = 3,
  kGrayAlpha = 4,
  kRgba = 6,
};

// The name of a kind of picture: "gray", "rgb", "palette", "gray+alpha",
// "rgba".
const char* PngColourName(PngColour colour) noexcept;

// What a picture's header says.
struct PngHeader {
  std::uint32_t width = 0;  // In pixels: from 1 to kMaxSide.
  std::uint32_t height = 0;
  std::uint8_t depth = 0;  // Bits a sample: 1, 2, 4, 8 or 16, as the kind of picture allows.
  PngColour colour = PngColour::kGray;
  bool interlaced = false;

  // How many samples a pixel has: 1 to 4.
  std::uint32_t Samples() const noexcept;
  // How many bytes a row's pixels take, before PNG's filter byte.
  std::uint64_t Stride() const noexcept;
};

// Reads one PNG picture after another. The rows it gives a RowSink stay as
// they are until the next call of Step().
class PngReader final {
 public:
  // The window it needs for reading rows: exactly this much, or more.
  static constexpr std::size_t kWindowBytes = core::Inflater::kWindowBytes;
  // The most of a chunk's bytes one Step() reads.
  static constexpr std::size_t kChunkBytes = 1024;
  // The most bytes of a row one Step() asks the inflater for.
  static constexpr std::size_t kRowBytesPerStep = 1024;
  // The most `IDAT` chunks with nothing in them that are passed over
  // between two that have something. No encoder writes one: none of the
  // 2,743 pictures in the books looked at has one.
  static constexpr unsigned kMaxEmptyChunksInARow = 2;
  // The widest and tallest a picture can be, as PNG has it.
  static constexpr std::uint32_t kMaxSide = 0x7FFFFFFF;

  // How much room the rows of a picture need: two rows, each the longer of
  // its bytes with the filter byte in front of them and its gray, which is
  // a byte a pixel. 0 for a header that is not one PNG allows.
  static std::uint64_t RowBytesFor(const PngHeader& header) noexcept;

  // The most bytes one Step() takes from its source, whatever the file.
  static constexpr std::size_t MostInputPerStep() noexcept {
    // A row's call: the compressed data the inflater may take, and for
    // each time it asks the source (no more often than the bytes it takes,
    // and once more), the ends and starts of as many chunks as may lie
    // between two of its bytes: a checksum and a chunk's start, 12 bytes.
    constexpr std::size_t kData = core::Inflater::MostInputFor(kRowBytesPerStep);
    constexpr std::size_t kRow = kData + ((kData + 1) * kChunkSeam * (kMaxEmptyChunksInARow + 1));
    // A chunk's call: a signature, a chunk's start, its bytes and its
    // checksum.
    constexpr std::size_t kChunk = kSignatureBytes + kChunkStartBytes + kChunkBytes + kChecksumBytes;
    return kRow > kChunk ? kRow : kChunk;
  }

  // `window` and `rows` are where the reader decompresses a picture's data
  // and keeps its rows. Both are the caller's and have to last as long as
  // the reader is used. Either may be empty, or too small, if only headers
  // will be read: a picture whose rows are asked for and do not fit is
  // refused with kNoRoom once its header has been read.
  PngReader(std::span<std::uint8_t> window, std::span<std::uint8_t> rows) noexcept;

  // It holds pointers into itself, so there is no copying one.
  PngReader(const PngReader&) = delete;
  PngReader& operator=(const PngReader&) = delete;
  ~PngReader() = default;

  // Gets ready to read a picture from `source`, which has to last until
  // the reading is done. With a sink, Step() reads on through the
  // picture's rows and gives each to it. Without one it stops once the
  // header has been read and checked: Header() then says what it says.
  // Reads nothing yet.
  void Begin(core::ByteSource& source, RowSink* sink) noexcept;

  // Does the next piece of the reading, a bounded amount (see above).
  // Returns true if there is more to do, false once the header has been
  // read (without a sink), every row has been given, or the reading has
  // failed: Status() says which.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  PngStatus Read(core::ByteSource& source, RowSink* sink) noexcept;

  PngStatus Status() const noexcept { return status_; }

  // Whether the header has been read and checked, and what it says.
  bool HeaderKnown() const noexcept { return headerKnown_; }
  const PngHeader& Header() const noexcept { return header_; }

  // Whether the picture says anything of transparency that is used: an
  // alpha sample, or a `tRNS` chunk that was taken.
  bool Transparent() const noexcept;

  // How many rows have been given to the sink.
  std::uint32_t RowsGiven() const noexcept { return rowsGiven_; }
  // How many chunks were passed over as not wanted, and how many of the
  // chunks the picture can do without had a wrong checksum and were
  // passed over for that.
  std::uint32_t ChunksPassedOver() const noexcept { return passedOver_; }
  std::uint32_t BadChecksumsPassedOver() const noexcept { return badPassedOver_; }

 private:
  static constexpr std::size_t kSignatureBytes = 8;
  static constexpr std::size_t kChunkStartBytes = 8;  // Its length and its name.
  static constexpr std::size_t kChecksumBytes = 4;
  static constexpr std::size_t kChunkSeam = kChecksumBytes + kChunkStartBytes;
  static constexpr std::size_t kHeaderBytes = 13;  // What `IHDR` holds.

  // The compressed data of a picture, which is in the `IDAT` chunks that
  // follow one another: their bytes one after another, as a source that
  // ends where the last of them does. Checks each chunk's checksum.
  class DataSource final : public core::ByteSource {
   public:
    // `length` is the length of the first `IDAT` chunk, whose start has
    // been read from `source`.
    void Start(core::ByteSource& source, std::uint32_t length) noexcept;
    [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override;

    // kOk unless a chunk went wrong, and what went wrong if one did. The
    // file ending is not a chunk going wrong: the data ends there, and the
    // inflater says whether that was too soon.
    PngStatus Status() const noexcept { return status_; }

   private:
    // Ends one chunk and starts the next. False once there is no more data.
    bool NextChunk() noexcept;
    bool Fail(PngStatus status) noexcept;

    core::ByteSource* source_ = nullptr;
    PngStatus status_ = PngStatus::kOk;
    bool ended_ = false;
    std::uint32_t left_ = 0;  // Bytes of the chunk being read still to come.
    std::uint32_t crc_ = 0;
    unsigned emptyInARow_ = 0;
  };

  enum class Stage : std::uint8_t {
    kSignature,   // Nothing is read yet.
    kChunkStart,  // A chunk begins next.
    kChunkData,   // In the middle of a chunk other than `IDAT`.
    kDataStart,   // The compressed data's first two bytes are next.
    kRows,        // In the rows.
    kDone,        // Finished, one way or another.
  };

  // What is done with a chunk once it has been read whole.
  enum class Chunk : std::uint8_t { kHeader, kPalette, kTransparency, kOther };

  bool ReadSignature() noexcept;
  bool StartChunk() noexcept;
  bool ReadChunkData() noexcept;
  bool StartData() noexcept;
  bool ReadRow() noexcept;
  // Reads `dst` whole from the source, or as much of it as there is.
  // False, with the status set, if the source failed or ended first.
  bool Fill(std::span<std::uint8_t> dst) noexcept;
  // A chunk has been read whole, its checksum good or not. `bytes` is
  // what it holds, if it is one that is kept.
  bool TakeChunk(bool sound, std::span<const std::uint8_t> bytes) noexcept;
  bool TakeHeader() noexcept;
  void TakePalette(std::span<const std::uint8_t> bytes) noexcept;
  void TakeTransparency(std::span<const std::uint8_t> bytes) noexcept;
  // The row in current_ is whole: undoes its filter and gives it out.
  bool FinishRow() noexcept;
  void Unfilter(std::uint8_t filter) noexcept;
  void ToGray(std::span<const std::uint8_t> row, std::span<std::uint8_t> gray) const noexcept;
  // Why the inflater stopped, as a status of the picture's.
  PngStatus DataFailure() const noexcept;
  bool Fail(PngStatus status) noexcept;
  bool Finish() noexcept;

  std::span<std::uint8_t> window_;
  std::span<std::uint8_t> rowMemory_;
  DataSource data_;
  core::Inflater inflater_;

  core::ByteSource* source_ = nullptr;
  RowSink* sink_ = nullptr;
  PngStatus status_ = PngStatus::kOk;
  Stage stage_ = Stage::kDone;

  PngHeader header_;
  bool headerKnown_ = false;
  bool seenPalette_ = false;
  bool seenTransparency_ = false;
  std::uint16_t paletteEntries_ = 0;
  // A gray or a colour that stands for "none", as the file's samples have
  // it, if `tRNS` gave one.
  bool hasKey_ = false;
  std::array<std::uint16_t, 3> key_{};
  // The palette as the grays its entries are on white paper, with their
  // alphas taken in. Black past the end of the palette.
  std::array<std::uint8_t, 256> paletteGray_{};

  // The chunk being read: what it is, how much of it is still to come,
  // and its checksum so far.
  Chunk chunk_ = Chunk::kOther;
  std::uint32_t chunkLeft_ = 0;
  std::uint32_t chunkLength_ = 0;
  std::uint32_t crc_ = 0;
  std::array<std::uint8_t, kHeaderBytes> headerBytes_{};

  // The two rows, each rowLength_ bytes: the one being read and the one
  // above it. A row is its filter byte and then its pixels' bytes; once it
  // has been undone, the row above has had its turn and its memory takes
  // the gray of this one.
  std::span<std::uint8_t> current_;
  std::span<std::uint8_t> previous_;
  std::size_t rowLength_ = 0;
  std::size_t stride_ = 0;  // Bytes of a row's pixels.
  std::size_t filled_ = 0;  // Bytes of the current row, its filter byte among them, read so far.
  std::uint32_t rowsGiven_ = 0;

  std::uint32_t passedOver_ = 0;
  std::uint32_t badPassedOver_ = 0;
};

}  // namespace bn::draw
