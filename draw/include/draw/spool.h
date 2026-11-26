#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "draw/fill.h"
#include "draw/fixed.h"

namespace bn::draw {

// Where a recording is kept: bytes added at its end, and read back from
// anywhere in it. The caller's: a file on a card, or memory. Each says
// whether it did all it was asked.
class SpoolStore {
 public:
  [[nodiscard]] virtual bool Append(std::span<const std::uint8_t> bytes) noexcept = 0;
  [[nodiscard]] virtual bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> bytes) noexcept = 0;

 protected:
  ~SpoolStore() = default;  // Never destroyed through the interface.
};

// How a recording went, or its giving back.
enum class SpoolStatus : std::uint8_t {
  kOk,
  kNoRoom,       // More chunks than the links given have room for: what came after is not kept.
  kStoreFailed,  // The store did not take a chunk, or give one back.
  kBadRecord,    // What was read back is not what was written.
};

// The name of a status, for diagnostics: "ok", "no room", "store failed",
// "bad record".
const char* SpoolStatusName(SpoolStatus status) noexcept;

// Keeps a drawing, so that it can be drawn into one strip of the picture
// after another without being drawn again for each.
//
// It is a Canvas, so a renderer can draw a whole picture into it once.
// Each line is placed as a Filler would place it, outlines left open are
// closed as a Filler would close them, and each line is kept for every band
// of the picture, `bandRows` rows high, whose rows it crosses: a Filler set
// to a band does nothing with a line that lies wholly above or below it.
// Each filling is kept for every band that has lines it uses up. So a
// band's lines and fillings are given back, in the order they came, by
// BeginBand() and ReplayStep(), and a Filler set to that band comes out the
// same, byte for byte, as if the drawing had been drawn into it.
//
// What a band keeps goes into a chunk of kChunkBytes in `buffers`, and
// each chunk, once full, is added to the store; the band's chunks are
// chained by `links`, two bytes for each chunk the store may hold. A line
// that goes on from where the band's last ended takes 5 bytes, or 9 if it
// is long; one that does not, 17; a filling 4 to 94.
//
// A drawing is drawn a part at a time, so a band's chunks mostly lie one
// after another in the store, and reading the store costs by the read more
// than by the byte. So chunks of a band that lie together are read
// together, as many as the memory BeginBand() is given holds: the links say
// which those are before anything is read, and nothing is read that is not
// the band's.
//
// Used by one task. The object is about a kilobyte; the memory for chunks
// and links is the caller's. ReplayStep() reads the store once at the most,
// for no more than that memory, gives back one chunk at the most, and fills
// once at the most.
class SpoolWriter final : public Canvas {
 public:
  static constexpr std::size_t kChunkBytes = 512;
  static constexpr std::size_t kMostBands = 64;
  // Chunks are numbered in two bytes, of which one number means none.
  static constexpr std::size_t kMostChunks = 65535;

  // How many bands a picture `height` rows high has, cut every `bandRows`,
  // and the bytes of `buffers` they need.
  static constexpr std::size_t BandsFor(std::int32_t height, std::int32_t bandRows) noexcept {
    return height <= 0 || bandRows <= 0 ? 0 : static_cast<std::size_t>((height + bandRows - 1) / bandRows);
  }
  static constexpr std::size_t BufferBytesFor(std::size_t bands) noexcept { return bands * kChunkBytes; }

  // Begins a recording of a picture `width` by `height` pixels, in bands of
  // `bandRows` rows. False, and nothing is kept, if there are no bands or
  // more than kMostBands, or `buffers` is too small, or there are no links.
  [[nodiscard]] bool Begin(std::int32_t width, std::int32_t height, std::int32_t bandRows,
                           std::span<std::uint8_t> buffers, std::span<std::uint16_t> links, SpoolStore& store) noexcept;
  // Adds what each band has in hand to the store. Then the recording can be
  // given back.
  SpoolStatus Finish() noexcept;
  SpoolStatus Status() const noexcept { return status_; }

  std::size_t Bands() const noexcept { return bands_; }
  // Chunks added to the store, the bytes they come to, and the lines and
  // fillings kept, each as many times as there are bands it is kept for.
  std::uint32_t Chunks() const noexcept { return chunks_; }
  std::uint64_t Bytes() const noexcept { return std::uint64_t{chunks_} * kChunkBytes; }
  std::uint64_t Lines() const noexcept { return lines_; }
  std::uint64_t Fills() const noexcept { return fills_; }

  void MoveTo(Point to) noexcept override;
  void LineTo(Point to) noexcept override;
  void Close() noexcept override;
  void Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity) noexcept override;
  void Fill(FillRule rule, std::uint8_t gray, std::uint8_t opacity, Point from, Point to) noexcept override;
  void Fill(FillRule rule, const GrayRamp& ramp, std::uint8_t opacity) noexcept override;
  void Fill(FillRule rule, const GrayRamp& ramp, std::uint8_t opacity, Point from, Point to) noexcept override;
  // The whole picture is wanted.
  std::int32_t Width() const noexcept override { return width_; }
  std::int32_t Top() const noexcept override { return 0; }
  std::int32_t Rows() const noexcept override { return height_; }

  // Begins giving back a band, reading its chunks into `memory`, which is
  // kChunkBytes or more: as many chunks as it holds whole are read at once,
  // where the band has that many together in the store. False if the
  // recording is not finished, or did not go right, or there is no such
  // band.
  [[nodiscard]] bool BeginBand(std::size_t band, std::span<std::uint8_t> memory) noexcept;
  // Gives some of the band back to `filler`, set to that band: the rest of
  // a chunk, or as far as a filling and that filling. True while there is
  // more; false when the band is all given back, or reading it back went
  // wrong, which Status() then says.
  [[nodiscard]] bool ReplayStep(Filler& filler) noexcept;

 private:
  using Place = Filler::Place;
  static constexpr std::uint16_t kNone = 0xFFFF;

  // What each band has in hand.
  struct Band {
    std::uint16_t head = kNone;  // Its first chunk in the store,
    std::uint16_t tail = kNone;  // and its last.
    std::uint16_t used = 0;      // Bytes of its chunk in hand.
    bool pending = false;        // Lines kept since its last filling.
    bool hasEnd = false;         // Where its last line ended:
    Place end;
  };

  void CloseOutline() noexcept;
  void Line(Place from, Place to) noexcept;
  void Record(std::size_t band, std::span<const std::uint8_t> record) noexcept;
  void Flush(std::size_t band) noexcept;
  // A filling, for each band with lines it uses up.
  void Filling(std::span<const std::uint8_t> record) noexcept;
  void Fail(SpoolStatus status) noexcept;

  SpoolStore* store_ = nullptr;
  std::span<std::uint8_t> buffers_;
  std::span<std::uint16_t> links_;
  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
  std::int32_t bandHeight_ = 0;  // In 256ths of a pixel.
  std::size_t bands_ = 0;
  std::array<Band, kMostBands> band_{};
  std::uint32_t chunks_ = 0;
  std::uint64_t lines_ = 0;
  std::uint64_t fills_ = 0;
  SpoolStatus status_ = SpoolStatus::kOk;
  bool recording_ = false;
  bool finished_ = false;
  Place start_;
  Place current_;

  // Giving a band back: the memory its chunks are read into, how many of
  // them are in it and which is in hand; that chunk, how far into it, the
  // next chunk that is not in memory, and where the last line given back
  // ended.
  std::span<std::uint8_t> memory_;
  std::size_t read_ = 0;
  std::size_t given_ = 0;
  std::span<std::uint8_t> chunk_;
  std::uint16_t next_ = kNone;
  std::size_t at_ = 0;
  bool loaded_ = false;
  bool replayHasEnd_ = false;
  Place replayEnd_;
};

}  // namespace bn::draw
