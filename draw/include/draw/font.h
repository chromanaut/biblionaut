#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The fonts image: every face the firmware draws text in, at every size it
// draws it, in one run of bytes that is read where it lies. On the device it
// is the flash partition called `fonts`, mapped into the processor's
// address space; on the desktop it is a file read into memory. `bn-font`
// (tools/font) makes it from font files.
//
// A face is one role (serif, sans, monospace, or the fallback for what the
// others lack), one style (regular, italic, bold, bold italic) and one size
// in pixels. Each has its metrics, a table from code points to glyphs, a
// record for each glyph, and the glyphs' pictures. Glyph 0 of a face is
// what is drawn for a character it has no glyph for.
//
// The image is not trusted. Opening it looks at its header and its table of
// faces, and nothing else; whatever a face's tables hold, finding a glyph
// looks at no more than kMostSteps of them, reads nothing outside the image,
// and gives a picture that lies wholly inside it, or no glyph.
//
// The layout, whose numbers are little-endian:
//
//   header, 16 bytes:
//     "BNFN"
//     version         1
//     0
//     faces     (2)   how many faces there are
//     bytes     (4)   how long the image is
//     0         (4)
//   a record for each face, 48 bytes, in no particular order:
//     role            0 serif, 1 sans, 2 monospace, 3 fallback
//     style           bit 0: bold; bit 1: italic
//     size      (2)   the size it was drawn at: pixels to the em
//     ascent    (2)   pixels from the baseline up to the top of the tallest
//                     letters, as the font gives it
//     descent   (2)   pixels from the baseline down to the bottom of the
//                     lowest, as the font gives it
//     line      (2)   pixels from one baseline to the next
//     x-height  (2)   pixels from the baseline to the top of an x
//     ranges    (4)   how many ranges of code points the face has,
//     at        (4)   and where the first is, from the image's start
//     glyphs    (4)   how many glyphs it has,
//     at        (4)   and where the first glyph's record is
//     pictures  (4)   where its pictures begin,
//     bytes     (4)   and how many bytes they take
//     0        (12)   kept for kerning
//   a range, 12 bytes: code points that have glyphs one after another
//     first     (4)   the first code point
//     count     (4)   how many
//     glyph     (4)   the glyph of the first
//   ranges are in order of their first code point, and do not overlap.
//   a glyph's record, 12 bytes:
//     advance   (2)   how far it moves the pen, in quarters of a pixel
//     width           its picture's width in pixels,
//     height          and height
//     left            pixels from the pen to the picture's left edge
//     top             pixels from the baseline up to the picture's top row
//     0         (2)   kept for kerning
//     picture   (4)   where its picture begins, from the face's pictures
//   a picture: its rows from the top, each pixel two bits, the first pixel
//   in the highest two bits of the first byte, rows following on with no
//   gap, the last byte filled out with nothing: 0 is paper, 3 full ink.

namespace bn::draw {

// How opening an image has gone.
enum class FontStatus : std::uint8_t {
  kOk = 0,
  kNotOpen,   // Open() has not been called.
  kBadImage,  // The bytes are not a fonts image, or say things outside it.
};

// The name of a status, for diagnostics: "ok", "not open", ...
const char* FontStatusName(FontStatus status) noexcept;

enum class FaceRole : std::uint8_t {
  kSerif = 0,
  kSans = 1,
  kMono = 2,
  kFallback = 3,
};

// A face's style, as bits.
inline constexpr std::uint8_t kBold = 1;
inline constexpr std::uint8_t kItalic = 2;

// One glyph, as a face has it.
struct Glyph {
  std::uint32_t index = 0;    // Which of the face's glyphs.
  std::uint16_t advance = 0;  // Quarters of a pixel.
  std::uint8_t width = 0;     // Its picture's size in pixels.
  std::uint8_t height = 0;
  std::int8_t left = 0;                   // Pixels from the pen to the picture's left edge,
  std::int8_t top = 0;                    // and from the baseline up to its top row.
  std::span<const std::uint8_t> picture;  // Inside the image; empty for a glyph with nothing to draw.

  // How much ink pixel (x, y) of the picture has: 0 for none, 3 for full.
  // The pixel has to be inside the picture.
  std::uint8_t Ink(std::uint32_t x, std::uint32_t y) const noexcept {
    const std::uint32_t bit = ((y * width) + x) * 2;
    return static_cast<std::uint8_t>((std::uint32_t{picture[bit / 8]} >> (6U - (bit % 8))) & 3U);
  }
};

// The bytes a picture `width` by `height` takes.
constexpr std::size_t PictureBytes(std::uint32_t width, std::uint32_t height) noexcept {
  return ((std::size_t{width} * height * 2) + 7) / 8;
}

// One face of an image. Has to be used only while the image's bytes are
// where they were when it was found.
class Face {
 public:
  // The most steps finding a glyph takes: looking at a range is a step.
  static constexpr std::uint32_t kMostSteps = 33;

  Face() noexcept = default;

  // Whether this is a face. One that is not has no glyphs.
  bool Valid() const noexcept { return !record_.empty(); }

  FaceRole Role() const noexcept;
  std::uint8_t Style() const noexcept;
  std::uint16_t Size() const noexcept;
  std::int16_t Ascent() const noexcept;
  std::int16_t Descent() const noexcept;
  std::uint16_t LineHeight() const noexcept;
  std::uint16_t XHeight() const noexcept;
  std::uint32_t Glyphs() const noexcept;

  // The glyph for `codePoint`, into `glyph`. False if the face has none, or
  // what its tables say of it is not to be had from the image.
  bool Find(std::uint32_t codePoint, Glyph& glyph) const noexcept;
  // Glyph `index`. False as Find() is.
  bool GlyphAt(std::uint32_t index, Glyph& glyph) const noexcept;
  // What is drawn for a character the face has no glyph for: glyph 0.
  bool Missing(Glyph& glyph) const noexcept { return GlyphAt(0, glyph); }

  // How many steps the last Find() took.
  std::uint32_t LastSteps() const noexcept { return steps_; }

 private:
  friend class FontImage;
  Face(std::span<const std::uint8_t> image, std::span<const std::uint8_t> record) noexcept
      : image_(image), record_(record) {}

  std::span<const std::uint8_t> image_;
  std::span<const std::uint8_t> record_;  // The face's 48 bytes.
  mutable std::uint32_t steps_ = 0;
};

// An image of faces.
class FontImage {
 public:
  // The most faces an image may have: one that says more is refused.
  static constexpr std::size_t kMostFaces = 1024;

  FontImage() noexcept = default;

  // Takes `image` as the fonts image. Its bytes are used where they lie,
  // and have to stay there for as long as faces of it are used.
  [[nodiscard]] FontStatus Open(std::span<const std::uint8_t> image) noexcept;
  FontStatus Status() const noexcept { return status_; }

  std::size_t Faces() const noexcept { return faces_; }
  // Face `index`, in the order the image has them.
  Face FaceAt(std::size_t index) const noexcept;
  // The face of `role` and `style` at `size` pixels, if the image has it;
  // a face that is not one otherwise.
  Face Find(FaceRole role, std::uint8_t style, std::uint16_t size) const noexcept;
  // The face of `role` nearest to `style` at `size`: the style itself if
  // the image has it, and otherwise bold italic gives way to bold and then
  // italic, and either to regular. A face that is not one if there is none
  // of `role` at `size`.
  Face Nearest(FaceRole role, std::uint8_t style, std::uint16_t size) const noexcept;

 private:
  std::span<const std::uint8_t> image_;
  std::size_t faces_ = 0;
  FontStatus status_ = FontStatus::kNotOpen;
};

}  // namespace bn::draw
