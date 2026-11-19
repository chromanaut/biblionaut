#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/byte_source.h"
#include "draw/row_sink.h"

// Reading a JPEG picture, as ITU-T T.81 has it.
//
// A picture is read from a ByteSource a piece at a time and never held
// whole: first its header, which says how large it is and what kind of
// picture, and then, if they are wanted, its rows, top first, each given to
// a RowSink. What is kept of the picture is one row of its blocks: the 8
// or 16 rows of pixels that are made at once.
//
// What is read:
//
//   - Baseline and extended pictures, coded with Huffman codes, 8 bits a
//     sample: gray (one component), and colour as YCbCr (three), sampled
//     in any way the standard allows, in one scan or in several.
//   - Progressive pictures, by their first scan alone (below).
//   - Restart markers.
//
// What is given out is 8-bit gray, a byte a pixel, from black (0) to white
// (255). A JPEG's gray is its first component, Y, which is 0.299 of red,
// 0.587 of green and 0.114 of blue (draw/gray.h): so only Y is turned back
// from frequencies into pixels. The two colour components are decoded as
// far as is needed to find where Y's next block begins, and no further.
//
// Smaller as it is read. A picture can be read at a half, a quarter or an
// eighth of its size each way (JpegReduction), for less work and less
// memory: each block of 8 by 8 gives 4 by 4 pixels, 2 by 2 or one. Each is
// the average of the pixels it stands for in the block at its whole size,
// worked out from the block's frequencies in one transform, never making
// the whole block; at an eighth that is the block's average alone, and its
// other frequencies are not even kept. Each side of the picture read is
// its side divided by the reduction, rounded up.
//
// Progressive pictures. A progressive picture is coded in scans over the
// whole of it, each adding to every block, and none of its pixels is
// whole until the last; keeping every block's coefficients between scans
// would take about two bytes a pixel. But its first scan holds each
// block's average, which is the picture at an eighth of its size, and is
// given a row of blocks at a time like any other. So a progressive picture
// is read at an eighth, whatever is asked, from the first scan that holds
// Y's averages, and the rest of its scans are not read. The low bits of
// an average that a later scan would refine are taken as nothing.
//
// What is not read: arithmetic coding, lossless and hierarchical JPEG,
// samples of 12 or 16 bits, four components (CMYK), colour coded as RGB (an
// Adobe marker before the frame header that says so, or components named
// R, G and B), a picture whose Y is sampled less than another component,
// and one whose height is given only after its first scan (`DNL`). Each of
// these has its header read and is refused with kUnsupported. Orientation
// written in an Exif block is not looked at.
//
// The file is not trusted. Whatever its bytes are, reading ends in a
// status. Between marker segments, bytes that are not a marker are passed
// over until one is (JunkBytes() counts them), as other readers do. Inside a
// scan's coded data, a code no table has, a coefficient past a block's
// end, or a restart marker other than the one due ends the reading with
// kBadData; coded data that runs into the end of the file, or into the end
// of the picture's marker, ends it with kTruncated. The rows of the blocks
// finished before that have been given; the row of blocks it was in is
// not.
//
// Memory. Room for one row of blocks (RowBytesFor()) is the caller's, and
// given when the reader is made; reading only the header needs none, and it
// may be empty for that. The object itself holds the tables, eight of
// Huffman codes and four of quantization, a block's numbers and a buffer of
// what it reads: 9,052 bytes on the device.
//
// One call's work. Step() does one of these and returns: it reads JPEG's
// first marker; or passes over up to kSegmentBytes of what lies between
// segments, finding a marker; or reads one table, a frame or scan header,
// or up to kSegmentBytes of a segment that is passed over; or passes over
// up to kSegmentBytes of a scan that is not read; or decodes up to
// kMcusPerStep of a scan's units of blocks (each at most 10 blocks), and
// turns Y's blocks among them into pixels; or gives one row to the sink.
// So the most it takes from its source, whatever the file, is
// MostInputPerStep(): a unit's blocks, each of whose 64 coefficients has
// the longest code and the most bits there can be, every byte of it 0xFF
// and so followed by a 0, and a restart marker after each unit; and what is
// read ahead into the reader's buffer, kInputBytes. Pictures as encoders
// write them take a few hundred bytes a call.
//
// Used by one task.

namespace bn::draw {

// How reading a picture has gone.
enum class JpegStatus : std::uint8_t {
  kOk = 0,        // Well so far, or to the end.
  kNotJpeg,       // It does not begin with JPEG's start-of-image marker.
  kBadMarker,     // A marker segment is shorter than a segment can be, or than what it holds; or a second
                  // start-of-image marker.
  kBadHeader,     // The frame header is not one JPEG allows, or there is a second one, or a scan or the end
                  // comes before it.
  kBadTable,      // A table of Huffman codes or of quantization is not one JPEG allows.
  kBadScan,       // A scan's header is not one JPEG allows, or names a component or a table that is not there.
  kUnsupported,   // A kind of JPEG this reader does not read (above). The header was read; no rows are given.
  kBadData,       // A scan's coded data is not what its tables allow.
  kTruncated,     // It ended before its last row. The rows before that were given.
  kNoRoom,        // The room for rows it was given is too small for this picture.
  kSourceFailed,  // The source reported an error.
};

// The name of a status, for diagnostics: "ok", "not jpeg", ...
const char* JpegStatusName(JpegStatus status) noexcept;

// How many times smaller each way a picture is read.
enum class JpegReduction : std::uint8_t {
  kNone = 1,
  kHalf = 2,
  kQuarter = 4,
  kEighth = 8,
};

// What a picture's header says.
struct JpegHeader {
  std::uint32_t width = 0;  // In pixels: from 1 to 65535.
  std::uint32_t height = 0;
  std::uint8_t components = 0;  // 1 (gray) or 3 (YCbCr), if it is read; what the frame says if not.
  bool progressive = false;
  // How Y is sampled, and how the colour components are, as the frame
  // header says: from 1 to 4 each way. The colour's are 0 for gray.
  std::uint8_t grayAcross = 0;
  std::uint8_t grayDown = 0;
  std::uint8_t colourAcross = 0;
  std::uint8_t colourDown = 0;
};

// The name of a kind of picture: "gray", or for colour how it is sampled
// ("4:4:4", "4:2:2", "4:2:0", "4:4:0", "4:1:1", or "ycbcr" for anything
// else). With "progressive" before it for a progressive picture.
const char* JpegKindName(const JpegHeader& header) noexcept;

// Reads one JPEG picture after another. The rows it gives a RowSink stay as
// they are until the next call of Step().
class JpegReader final {
 public:
  // The most of a segment, or of what is passed over, that one Step() reads.
  static constexpr std::size_t kSegmentBytes = 1024;
  // The most units of blocks one Step() decodes.
  static constexpr std::size_t kMcusPerStep = 8;
  // The most blocks in a unit, as the standard has it.
  static constexpr std::size_t kMostBlocksPerMcu = 10;
  // How much is read from the source at a time.
  static constexpr std::size_t kInputBytes = 512;
  // The most fill bytes (0xFF) before a marker in a scan's coded data.
  static constexpr std::size_t kMostFillBytes = 16;

  // The reduction a picture is best read at to be shown at a size: the
  // largest that still leaves it at least that large both ways. An eighth
  // for a progressive picture, whatever the size.
  static JpegReduction ReductionFor(const JpegHeader& header, std::uint32_t shownWidth,
                                    std::uint32_t shownHeight) noexcept;
  // What a reduction becomes for a picture: an eighth if it is progressive.
  static JpegReduction ReductionTaken(const JpegHeader& header, JpegReduction asked) noexcept;
  // The picture's sides as it is read at a reduction (ReductionTaken()):
  // each divided by it, rounded up.
  static std::uint32_t WidthAt(const JpegHeader& header, JpegReduction asked) noexcept;
  static std::uint32_t HeightAt(const JpegHeader& header, JpegReduction asked) noexcept;

  // How much room the rows of a picture need at a reduction: a row of its
  // blocks, as wide as its units of blocks are across, at the size read. 0
  // for a header that is not one this reader reads.
  static std::uint64_t RowBytesFor(const JpegHeader& header, JpegReduction asked) noexcept;

  // The most bytes one Step() takes from its source, whatever the file.
  static constexpr std::size_t MostInputPerStep() noexcept {
    // A block: its average's longest code and most bits (16 and 11), and
    // 63 coefficients' (16 and 10); every byte doubled.
    constexpr std::size_t kBlockBits = (16 + 11) + (63 * (16 + 10));
    constexpr std::size_t kBlockBytes = 2 * ((kBlockBits + 7) / 8);
    // A unit, and the restart marker after it, with its fill bytes.
    constexpr std::size_t kMcu = (kMostBlocksPerMcu * kBlockBytes) + kMostFillBytes + 2;
    constexpr std::size_t kRows = kMcusPerStep * kMcu;
    // A segment's call: a marker and up to kSegmentBytes.
    constexpr std::size_t kSegment = kSegmentBytes + 4;
    return (kRows > kSegment ? kRows : kSegment) + kInputBytes;
  }

  // `rows` is where the reader keeps a row of blocks. It is the caller's and
  // has to last as long as the reader is used. It may be empty, or too
  // small, if only headers will be read: a picture whose rows are asked for
  // and do not fit is refused with kNoRoom once its header has been read.
  explicit JpegReader(std::span<std::uint8_t> rows) noexcept;

  JpegReader(const JpegReader&) = delete;
  JpegReader& operator=(const JpegReader&) = delete;
  ~JpegReader() = default;

  // Gets ready to read a picture from `source`, which has to last until the
  // reading is done, at a reduction (taken as an eighth for a progressive
  // picture). With a sink, Step() reads on through the picture's rows and
  // gives each to it. Without one it stops once the header has been read
  // and checked: Header() then says what it says. Reads nothing yet.
  void Begin(core::ByteSource& source, RowSink* sink, JpegReduction reduction = JpegReduction::kNone) noexcept;

  // Does the next piece of the reading, a bounded amount (see above).
  // Returns true if there is more to do, false once the header has been
  // read (without a sink), every row has been given, or the reading has
  // failed: Status() says which.
  [[nodiscard]] bool Step() noexcept;

  // Begin(), and Step() until there is no more.
  JpegStatus Read(core::ByteSource& source, RowSink* sink, JpegReduction reduction = JpegReduction::kNone) noexcept;

  JpegStatus Status() const noexcept { return status_; }

  // Whether the header has been read and checked, and what it says.
  bool HeaderKnown() const noexcept { return headerKnown_; }
  const JpegHeader& Header() const noexcept { return header_; }

  // The reduction the picture is read at, and its size read so: once the
  // header is known.
  JpegReduction Reduction() const noexcept { return reduction_; }
  std::uint32_t Width() const noexcept { return outWidth_; }
  std::uint32_t Height() const noexcept { return outHeight_; }

  // How many rows have been given to the sink.
  std::uint32_t RowsGiven() const noexcept { return rowsGiven_; }
  // How many marker segments were passed over as not wanted; how many
  // scans were; and how many bytes between segments were not a marker.
  std::uint32_t SegmentsPassedOver() const noexcept { return segmentsPassedOver_; }
  std::uint32_t ScansPassedOver() const noexcept { return scansPassedOver_; }
  std::uint32_t JunkBytes() const noexcept { return junkBytes_; }

 private:
  static constexpr std::size_t kMostComponents = 4;

  struct Huffman {
    bool defined = false;
    // The values, in the order of their codes.
    std::array<std::uint8_t, 256> values{};
    // For each length of code from 1 to 16: the largest code of that
    // length (-1 if none), and where its values begin less its first code.
    std::array<std::int32_t, 18> largest{};
    std::array<std::int32_t, 17> offset{};
    // For each 8 bits that may come next: the length of the code they begin
    // with if it is 8 bits or fewer (0 if not), and its value.
    std::array<std::uint8_t, 256> fastLength{};
    std::array<std::uint8_t, 256> fastValue{};
  };

  struct Component {
    std::uint8_t id = 0;
    std::uint8_t across = 0;  // Sampling, 1 to 4.
    std::uint8_t down = 0;
    std::uint8_t table = 0;  // Of quantization.
    // In the scan being decoded: its tables of codes, and its last average.
    std::uint8_t dcTable = 0;
    std::uint8_t acTable = 0;
    std::int32_t last = 0;
  };

  enum class Stage : std::uint8_t {
    kStart,     // Nothing is read yet.
    kMarker,    // A marker is looked for next.
    kSegment,   // In a marker segment.
    kSkipScan,  // In a scan's coded data that is not read.
    kDecode,    // In the coded data of the scan that is read.
    kGive,      // A row of blocks is made, and its rows are being given.
    kDone,      // Finished, one way or another.
  };

  // What the segment being read is.
  enum class Segment : std::uint8_t { kPass, kHuffman, kQuantization, kFrame, kScan, kRestart, kAdobe };

  bool ReadStart() noexcept;
  bool FindMarker() noexcept;
  bool TakeMarker(std::uint8_t marker) noexcept;
  bool ReadSegment() noexcept;
  bool ReadFrame(std::uint8_t marker, std::size_t length) noexcept;
  bool ReadScan(std::size_t length) noexcept;
  bool ReadRestartInterval(std::size_t length) noexcept;
  bool ReadAdobe(std::size_t length) noexcept;
  bool ReadHuffman() noexcept;
  bool ReadQuantization() noexcept;
  bool SkipScan() noexcept;
  bool Decode() noexcept;
  bool Give() noexcept;
  // Gets ready to decode the scan whose header has just been read.
  bool StartRows() noexcept;
  // One unit of blocks; false, with the status set, if its data is bad.
  bool DecodeMcu() noexcept;
  bool DecodeBlock(Component& component, bool wanted) noexcept;
  bool DecodeAverage(Component& component, bool wanted) noexcept;
  // Turns the block's numbers into pixels at a place in the row of blocks.
  void MakePixels(std::uint32_t blockX, std::uint32_t blockY) noexcept;
  // At the end of a restart interval: the marker due, and the averages
  // begun again. What went wrong if it is not there.
  JpegStatus Restart() noexcept;

  // Bytes from the source, through the buffer. Byte() is false at the end
  // of the source or if it failed, with ended_ or failed_ set.
  bool Byte(std::uint8_t& out) noexcept;
  // Reads `dst` whole, counting it against the segment. False, with the
  // status set, if the source failed or ended first.
  bool Fill(std::span<std::uint8_t> dst) noexcept;
  // Bits of a scan's coded data, the byte after a 0xFF taken out. Past the
  // end of the data (a marker, or the end of the file) the bits are 0, and
  // taking one of those marks the data as run out.
  void MoreBits() noexcept;
  std::uint32_t PeekBits(unsigned count) noexcept;
  void SkipBits(unsigned count) noexcept;
  std::int32_t ReceiveExtend(unsigned count) noexcept;
  // The next value by a table of codes; -1 if no code of the table begins
  // there.
  std::int32_t DecodeValue(const Huffman& table) noexcept;
  // Bits begun again at a byte's start, as at a restart marker.
  void ResetBits() noexcept;

  bool Fail(JpegStatus status) noexcept;
  bool Finish() noexcept;

  std::span<std::uint8_t> rowMemory_;
  core::ByteSource* source_ = nullptr;
  RowSink* sink_ = nullptr;
  JpegStatus status_ = JpegStatus::kOk;
  Stage stage_ = Stage::kDone;
  JpegReduction asked_ = JpegReduction::kNone;
  JpegReduction reduction_ = JpegReduction::kNone;

  // The source, through a buffer.
  std::array<std::uint8_t, kInputBytes> input_{};
  std::size_t inputAt_ = 0;
  std::size_t inputEnd_ = 0;
  bool ended_ = false;
  bool failed_ = false;
  // A 0xFF has been read, and what follows it is not yet known.
  bool sawFF_ = false;

  JpegHeader header_;
  bool headerKnown_ = false;
  bool frameSeen_ = false;
  std::uint8_t frameMarker_ = 0;
  std::int32_t adobeTransform_ = -1;  // As an Adobe marker gives it; -1 without one.
  std::uint16_t restartInterval_ = 0;
  std::uint8_t componentCount_ = 0;
  std::array<Component, kMostComponents> components_{};
  std::array<Huffman, 8> huffman_{};                             // Four for averages, then four for the rest.
  std::array<std::array<std::uint16_t, 64>, 4> quantization_{};  // In the order coefficients are coded.
  std::array<bool, 4> quantizationDefined_{};

  // The segment being read.
  Segment segment_ = Segment::kPass;
  std::size_t segmentLeft_ = 0;

  // The scan that is read: its components in order, and which of them is Y.
  std::array<std::uint8_t, kMostComponents> scanComponents_{};
  std::uint8_t scanCount_ = 0;
  std::uint8_t successive_ = 0;  // Al: how far the averages of a progressive scan are shifted.
  bool interleaved_ = false;
  std::uint32_t mcusAcross_ = 0;
  std::uint32_t mcusDown_ = 0;
  std::uint32_t mcuX_ = 0;
  std::uint32_t mcuY_ = 0;
  std::uint32_t mcusToRestart_ = 0;
  std::uint8_t nextRestart_ = 0;

  // The bits of the coded data: `bitCount_` of them at the top of `bits_`,
  // the last `zeroBits_` of which are past the end of the data.
  std::uint32_t bits_ = 0;
  unsigned bitCount_ = 0;
  unsigned zeroBits_ = 0;
  bool dataEnded_ = false;       // A marker or the end of the file has been met.
  std::uint8_t dataMarker_ = 0;  // The marker met, 0 for the end of the file.
  bool ranOut_ = false;          // A bit past the end of the data has been taken.

  // A row of blocks, as pixels: `rowsPerMcu_` rows of `bufferWidth_`.
  std::uint32_t blockSide_ = 8;  // Pixels a block gives each way: 8 divided by the reduction.
  std::uint32_t bufferWidth_ = 0;
  std::uint32_t rowsPerMcu_ = 0;
  std::uint32_t outWidth_ = 0;
  std::uint32_t outHeight_ = 0;
  std::uint32_t rowsGiven_ = 0;
  std::uint32_t rowsMade_ = 0;  // Of the picture, by the rows of blocks made so far.

  // A block's coefficients, in their places, and the half-made pixels;
  // whether any but its average was given, and the furthest right and down
  // of those that were.
  std::array<std::int32_t, 64> block_{};
  std::array<std::int32_t, 64> work_{};
  bool acSeen_ = false;
  std::uint32_t lastAcross_ = 0;
  std::uint32_t lastDown_ = 0;

  std::uint32_t segmentsPassedOver_ = 0;
  std::uint32_t scansPassedOver_ = 0;
  std::uint32_t junkBytes_ = 0;
};

}  // namespace bn::draw
