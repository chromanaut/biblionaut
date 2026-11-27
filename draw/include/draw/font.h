#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/byte_map.h"

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
// A face may kern: bring two of its glyphs that follow one another closer
// together, or set them further apart, than their advances would. Its
// glyphs are put in classes for that, twice over. Those that kern alike
// with whatever follows them are one class as the first of two, and those
// that kern alike after whatever comes before them are one class as the
// second; and the face has a table with a number for each class of the one
// kind and each of the other. A glyph that never kerns as the first of two
// is of class 0 there, and likewise as the second, and the table has
// nothing for class 0.
//
// A face may have ligatures: a glyph it draws in place of two or three of
// its glyphs that follow one another, as one for an f and an i. It has a
// list of them, each the glyphs it is made of and the glyph that stands for
// them, which is a glyph of the face like any other: it may be some
// character's own, or one that no character has, which has a number after
// those of the characters' glyphs; and it kerns by its own classes. Which
// glyphs a face joins is its font's affair, and nothing here knows of any
// letters.
//
// The image is not trusted. Opening it looks at its header and its table of
// faces, and nothing else; whatever a face's tables hold, finding a glyph
// looks at no more than kMostSteps of them, reads nothing outside the image,
// and gives a picture that lies wholly inside it, or no glyph.
//
// An image can be opened where all of it can be read at once, as a span of
// bytes, or through a core::ByteMap, for a device that has not the address
// space to map the whole of an image. Opened so, only its header and its
// table of faces are kept readable. A face says its role, style, size and
// metrics from the first, and has glyphs once it is held
// (FontImage::Hold()), which makes readable the one stretch of the image
// that has its tables, kerning and ligatures among them, and its pictures:
// `bn-font` writes a face's together, and an image that scatters them asks
// for more than may be had. So the faces a page is set in are held, and
// given up for the next page's.
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
//     kerning   (4)   where its table of kerning begins,
//     firsts          how many classes its glyphs are in as the first of
//                     two that kern, the class of those that do not left
//                     out,
//     seconds         and how many as the second: 0 of both for a face
//                     with no kerning
//     ligatures       how many ligatures it has,
//     0
//     at        (4)   and where the first is
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
//     first           its class as the first of two glyphs that kern: 0
//                     if it never does, and 1 and up otherwise
//     second          its class as the second of two, likewise
//     picture   (4)   where its picture begins, from the face's pictures
//   a table of kerning: for each class of first glyphs from 1 up, in order,
//   a signed byte for each class of second glyphs from 1 up: how much
//   further on the second is set, in quarters of a pixel. Less than nothing
//   brings the two together.
//   a ligature, 8 bytes:
//     first     (2)   the glyphs it is made of, as they follow one another:
//     second    (2)   the first two,
//     third     (2)   and the third, or 0 for a ligature of two
//     glyph     (2)   the glyph that is drawn in their place
//   where a ligature of three begins as one of two does, whichever comes
//   first in the list is the one that is made: `bn-font` keeps the order
//   the font tries its own in.
//   a picture: its rows from the top, each pixel two bits, the first pixel
//   in the highest two bits of the first byte, rows following on with no
//   gap, the last byte filled out with nothing: 0 is paper, 3 full ink.

namespace bn::draw {

// How opening an image has gone.
enum class FontStatus : std::uint8_t {
  kOk = 0,
  kNotOpen,   // Open() has not been called.
  kBadImage,  // The bytes are not a fonts image, or say things outside it.
  kNoRoom,    // Its header and table of faces could not be had to read.
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
  std::uint32_t pictureAt = 0;            // Where in the image the picture begins, from the image's start.
  // Its classes for kerning, as the first of two glyphs and as the second:
  // 0 for one that does not kern. They mean something to its own face only,
  // which is `face` of the image's faces.
  std::uint8_t kernFirst = 0;
  std::uint8_t kernSecond = 0;
  std::uint16_t face = 0;

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
// where they were when it was found, and its glyphs only until the image's
// faces are released.
class Face {
 public:
  // The most steps finding a glyph takes: looking at a range is a step.
  static constexpr std::uint32_t kMostSteps = 33;

  Face() noexcept = default;

  // Whether this is a face. One that is not has no glyphs.
  bool Valid() const noexcept { return !record_.empty(); }
  // Whether its tables and pictures can be read. A face of an image opened
  // as a span always is; one of an image opened through a core::ByteMap is
  // once FontImage::Hold() has been given it. A face that is not held has
  // its metrics and no glyphs.
  bool Held() const noexcept { return held_; }

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

  // How much further on `second` is set when it follows `first`, than the
  // advance of `first` puts it: in quarters of a pixel, and less than
  // nothing when the two are brought together. Nothing unless both are
  // glyphs this face gave, and it kerns the two. It reads one byte of the
  // image.
  std::int8_t Kern(const Glyph& first, const Glyph& second) const noexcept;
  // How many classes its glyphs are in for kerning, as the first of two and
  // as the second, and how many bytes its table is: 0 of each for a face
  // that does not kern.
  std::uint8_t KernFirsts() const noexcept;
  std::uint8_t KernSeconds() const noexcept;
  std::size_t KerningBytes() const noexcept { return std::size_t{KernFirsts()} * KernSeconds(); }

  // The glyph the face draws in place of the first two or three of
  // `glyphs`, which follow one another in a line, if it has a ligature of
  // them: into `ligature`, and how many glyphs it stands for. 0, with
  // `ligature` left as it was, if it has none of them, if there are fewer
  // than two, if the first two are not both glyphs this face gave, or if
  // the glyph its list names cannot be had. The first of its ligatures that
  // fits is the one made. It looks at each of them once at the most: 255,
  // whatever the image holds.
  std::size_t Ligature(std::span<const Glyph> glyphs, Glyph& ligature) const noexcept;
  // How many ligatures it has.
  std::uint8_t Ligatures() const noexcept;
  // A mark for the glyphs that begin a ligature, for whoever goes through
  // many glyphs and would not go through the list for each: bit n is set if
  // a glyph whose number is n more than a multiple of 64 begins one. So a
  // glyph whose bit is not set begins none, and one whose bit is set may.
  // It goes through the list once. Nothing is set for a face that is not
  // held.
  std::uint64_t LigatureFirsts() const noexcept;

  // How many steps the last Find() took.
  std::uint32_t LastSteps() const noexcept { return steps_; }

 private:
  friend class FontImage;
  Face(std::span<const std::uint8_t> record, std::size_t index) noexcept : record_(record), index_(index) {}

  // The bytes of the image from `at`, which whoever asks has seen to be
  // among the face's own.
  std::span<const std::uint8_t> From(std::size_t at) const noexcept { return bytes_.subspan(at - base_); }

  std::span<const std::uint8_t> record_;  // The face's 48 bytes.
  std::size_t index_ = 0;                 // Which of the image's faces.
  // What can be read of the image for this face, and where in the image
  // that begins: all of it from 0, or the face's own stretch.
  std::span<const std::uint8_t> bytes_;
  std::size_t base_ = 0;
  bool held_ = false;
  mutable std::uint32_t steps_ = 0;
};

// An image of faces.
class FontImage {
 public:
  // The most faces an image may have: one that says more is refused.
  static constexpr std::size_t kMostFaces = 1024;
  // The most faces of an image opened through a core::ByteMap that are
  // held at once.
  static constexpr std::size_t kMostHeld = 16;

  FontImage() noexcept = default;
  ~FontImage() { Close(); }
  FontImage(const FontImage&) = delete;
  FontImage& operator=(const FontImage&) = delete;

  // Takes `image` as the fonts image. Its bytes are used where they lie,
  // and have to stay there for as long as faces of it are used. Every face
  // is held from the first.
  [[nodiscard]] FontStatus Open(std::span<const std::uint8_t> image) noexcept;
  // Takes the bytes of `map` as the fonts image, and keeps readable only
  // its header and its table of faces: 49,168 bytes at the most. `map` has
  // to outlast the image, or its Close().
  [[nodiscard]] FontStatus Open(core::ByteMap& map) noexcept;
  // Gives up everything the image has readable. It is then not open.
  void Close() noexcept;
  FontStatus Status() const noexcept { return status_; }

  // Has `face`'s tables and pictures readable, so that it has glyphs. True
  // if they are: as they always are in an image opened as a span. False if
  // `face` is not a face of this image, if kMostHeld others are held, or if
  // there is no room for its stretch of the image, and then nothing has
  // changed. A face found again while it is held is held as found.
  [[nodiscard]] bool Hold(Face& face) noexcept;
  // Gives up every face held. Their glyphs, and the faces themselves, have
  // not to be used afterwards: find the faces again. Nothing in an image
  // opened as a span.
  void ReleaseFaces() noexcept;
  // How many faces are held, and how many bytes of the image they come to;
  // 0 of both in an image opened as a span.
  std::size_t Held() const noexcept { return held_; }
  std::size_t HeldBytes() const noexcept;
  // The `length` bytes of the image from `at`, if they are readable: any of
  // an image opened as a span, and those of a held face otherwise. Empty if
  // not. It is how a picture is had again from Glyph::pictureAt.
  std::span<const std::uint8_t> Bytes(std::size_t at, std::size_t length) const noexcept;

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
  // A face held: which, the stretch of the image that is its, and where in
  // the image the stretch begins.
  struct HeldFace {
    std::size_t index = 0;
    std::span<const std::uint8_t> bytes;
    std::size_t base = 0;
  };

  // The header and the table of faces; in an image opened as a span, all
  // of the image.
  std::span<const std::uint8_t> head_;
  core::ByteMap* map_ = nullptr;  // Null in an image opened as a span.
  std::array<HeldFace, kMostHeld> heldFaces_{};
  std::size_t held_ = 0;
  std::size_t faces_ = 0;
  FontStatus status_ = FontStatus::kNotOpen;
};

}  // namespace bn::draw
