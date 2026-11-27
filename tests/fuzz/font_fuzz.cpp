// The fuzz target for the reader of the fonts image (draw/font.h).
//
// What it is given:
//
//   byte 0        bit 0: the image is made to say it is as long as it is,
//                 so that tables near its end are in it
//                 bit 1: the map it is opened through a second time has
//                 room for only so many eighths of it at once, one more
//                 than bits 2 to 4 say
//   bytes 1 to 8  four code points to ask each face for, two bytes each
//   the rest      a fonts image, or what passes for one
//
// What has to hold, whatever the bytes are:
//
//   - opening the same bytes twice goes the same way, and an image that is
//     refused has no faces;
//   - a face is one of the four roles and the four styles, and is found by
//     its own role, style and size;
//   - the nearest face to each style, of a face's role at its size, is of
//     the first style the image has among the style itself, its bold, its
//     italic and regular, found here by going through the faces;
//   - finding a glyph takes no more steps than halving the face's ranges
//     does, and never more than Face::kMostSteps;
//   - a glyph that is given has a picture of the size its width and height
//     make, wholly inside the image, every pixel of which can be read; and
//     is the glyph its number gives;
//   - in a face whose ranges are in order and apart, as the layout says
//     they are, a code point has a glyph exactly when going through the
//     ranges one by one finds it in one, and the glyph is the one the
//     image's bytes say, read here the plain way;
//   - a glyph has the classes for kerning its record says, and its face's
//     number; every two glyphs found in a face are kerned by what the
//     image's bytes say, read here the plain way, which is nothing if
//     either is of class 0 or of a class the face's table has not; and a
//     glyph that says it is another face's is not kerned at all;
//   - of every two and three glyphs found in a face, it makes the ligature
//     the image's bytes say, read here the plain way: the first in its list
//     that is of those glyphs, with a glyph that can be had; the glyph it
//     gives for one is as much inside the image as any; and it makes none
//     of a glyph that says it is another face's;
//   - opened through a map (core/byte_map.h), an image that opens as a span
//     opens, unless the map has not room for its table of faces, and has
//     only that table mapped; a face has the same numbers, and no glyph
//     until it is held; it is held exactly when there is room for its
//     stretch of the image, from the first of its tables to the end of the
//     last, its kerning and its ligatures among them, and that stretch is
//     all that is mapped for it; held, it gives for a code point or a number
//     the glyph the image opened as a span gives, with the same picture, and
//     kerns two glyphs and joins two or three as that does; given up, it has
//     none again; and nothing is left mapped when the image is gone.
//
// The image is in memory of exactly its size, so the sanitizer sees a byte
// read outside it; and each stretch a map gives is a copy of exactly its
// size, freed when it is given up, so it sees a byte read outside a face's
// stretch, or after it.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "byte_map_fakes.h"
#include "draw/font.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Face;
using bn::draw::FontImage;
using bn::draw::FontStatus;
using bn::draw::Glyph;

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFaceBytes = 48;
constexpr std::size_t kRangeBytes = 12;
constexpr std::size_t kGlyphBytes = 12;
// Faces looked into: an image may have a thousand, all the same few bytes.
constexpr std::size_t kFacesTried = 12;

std::uint32_t Get16(const Bytes& from, std::size_t at) {
  return std::uint32_t{from[at]} | (std::uint32_t{from[at + 1]} << 8U);
}

std::uint32_t Get32(const Bytes& from, std::size_t at) { return Get16(from, at) | (Get16(from, at + 2) << 16U); }

struct Range {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  std::uint32_t glyph = 0;
};

// A face's ranges, read the plain way. Opening the image has seen that
// they are inside it.
std::vector<Range> RangesOf(const Bytes& image, std::size_t record) {
  const std::size_t count = Get32(image, record + 12);
  const std::size_t at = Get32(image, record + 16);
  std::vector<Range> ranges;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t range = at + (i * kRangeBytes);
    ranges.push_back({Get32(image, range), Get32(image, range + 4), Get32(image, range + 8)});
  }
  return ranges;
}

// Whether they are as the layout says: in order, apart, none empty or
// running past the last number there is.
bool InOrder(const std::vector<Range>& ranges) {
  std::uint64_t free = 0;  // The first code point not yet taken.
  for (const Range& range : ranges) {
    if (range.count == 0 || range.first < free) {
      return false;
    }
    free = std::uint64_t{range.first} + range.count;
    if (free > std::uint64_t{1} << 32U) {
      return false;
    }
  }
  return true;
}

void CheckGlyph(const Bytes& image, const Face& face, const Glyph& glyph) {
  BN_FUZZ_CHECK(glyph.index < face.Glyphs(), "a glyph was given that the face does not have");
  BN_FUZZ_CHECK(glyph.picture.size() == bn::draw::PictureBytes(glyph.width, glyph.height),
                "a picture is not the size its width and height make");
  if (!glyph.picture.empty()) {
    BN_FUZZ_CHECK(glyph.picture.data() >= image.data() &&
                      glyph.picture.data() + glyph.picture.size() <= image.data() + image.size(),
                  "a picture lies outside the image");
  }
  std::uint32_t ink = 0;
  for (std::uint32_t y = 0; y < glyph.height; ++y) {
    for (std::uint32_t x = 0; x < glyph.width; ++x) {
      ink += glyph.Ink(x, y);
    }
  }
  BN_FUZZ_CHECK(ink <= 3U * glyph.width * glyph.height, "a pixel has more ink than there is");
  Glyph again;
  BN_FUZZ_CHECK(face.GlyphAt(glyph.index, again), "a glyph that was found is not the one its number gives");
  BN_FUZZ_CHECK(again.advance == glyph.advance && again.width == glyph.width && again.height == glyph.height &&
                    again.left == glyph.left && again.top == glyph.top && again.picture.data() == glyph.picture.data(),
                "a glyph that was found is not the one its number gives");
}

// Whether the image's bytes, read the plain way, say glyph `index` of the
// face at `record` can be had; and if so, that `glyph` is it.
bool PlainGlyph(const Bytes& image, std::size_t record, std::uint64_t index, const Glyph* glyph) {
  if (index >= Get32(image, record + 20)) {
    return false;
  }
  const std::size_t at = Get32(image, record + 24) + (static_cast<std::size_t>(index) * kGlyphBytes);
  const std::uint64_t picture = Get32(image, at + 8);
  const std::uint64_t bytes = bn::draw::PictureBytes(image[at + 2], image[at + 3]);
  if (picture + bytes > Get32(image, record + 32)) {
    return false;
  }
  if (glyph != nullptr) {
    BN_FUZZ_CHECK(glyph->index == index && glyph->advance == Get16(image, at) && glyph->width == image[at + 2] &&
                      glyph->height == image[at + 3] && glyph->left == static_cast<std::int8_t>(image[at + 4]) &&
                      glyph->top == static_cast<std::int8_t>(image[at + 5]),
                  "a glyph is not what the image's bytes say");
    if (bytes > 0) {
      BN_FUZZ_CHECK(glyph->picture.data() == image.data() + Get32(image, record + 28) + picture,
                    "a picture is not where the image's bytes say");
    }
    // Its ink, a pixel at a time: two bits each, the first highest.
    const std::size_t pictureAt = Get32(image, record + 28) + static_cast<std::size_t>(picture);
    for (std::uint32_t pixel = 0; pixel < std::uint32_t{glyph->width} * glyph->height; ++pixel) {
      const std::uint32_t ink = (std::uint32_t{image[pictureAt + (pixel / 4)]} >> (6U - ((pixel % 4) * 2))) & 3U;
      BN_FUZZ_CHECK(glyph->Ink(pixel % glyph->width, pixel / glyph->width) == ink,
                    "a pixel's ink is not what the image's bytes say");
    }
  }
  return true;
}

// What the image's bytes, read the plain way, say glyph `second` of the
// face at `record` is kerned by after its glyph `first`. Both are glyphs
// the face has.
int PlainKerning(const Bytes& image, std::size_t record, std::uint32_t first, std::uint32_t second) {
  const std::size_t glyphs = Get32(image, record + 24);
  const unsigned firsts = image[record + 40];
  const unsigned seconds = image[record + 41];
  const unsigned row = image[glyphs + (std::size_t{first} * kGlyphBytes) + 6];
  const unsigned column = image[glyphs + (std::size_t{second} * kGlyphBytes) + 7];
  if (row == 0 || row > firsts || column == 0 || column > seconds) {
    return 0;
  }
  return static_cast<std::int8_t>(image[Get32(image, record + 36) + (std::size_t{row - 1} * seconds) + (column - 1)]);
}

// The most glyphs of a face that are kerned with one another here, and
// joined.
constexpr std::size_t kGlyphsKerned = 12;

constexpr std::size_t kLigatureBytes = 8;

// Every two of `glyphs`, which are the face's, are kerned as the image's
// bytes say; and none with a glyph that says it is another face's.
void CheckKerning(const Bytes& image, const Face& face, std::size_t number, const std::vector<Glyph>& glyphs) {
  const std::size_t record = kHeaderBytes + (number * kFaceBytes);
  BN_FUZZ_CHECK(face.KernFirsts() == image[record + 40] && face.KernSeconds() == image[record + 41] &&
                    face.KerningBytes() == std::size_t{image[record + 40]} * image[record + 41],
                "a face's classes for kerning are not the image's");
  const std::size_t records = Get32(image, record + 24);
  for (const Glyph& first : glyphs) {
    BN_FUZZ_CHECK(first.face == number &&
                      first.kernFirst == image[records + (std::size_t{first.index} * kGlyphBytes) + 6] &&
                      first.kernSecond == image[records + (std::size_t{first.index} * kGlyphBytes) + 7],
                  "a glyph's classes for kerning, or its face, are not what the image's bytes say");
    for (const Glyph& second : glyphs) {
      BN_FUZZ_CHECK(face.Kern(first, second) == PlainKerning(image, record, first.index, second.index),
                    "two glyphs are not kerned by what the image's bytes say");
      Glyph stranger = second;
      stranger.face = static_cast<std::uint16_t>(second.face + 1U);
      BN_FUZZ_CHECK(face.Kern(first, stranger) == 0 && face.Kern(stranger, second) == 0,
                    "a glyph of another face is kerned");
    }
  }
}

// What the image's bytes, read the plain way, say the face at `record`
// draws in place of its glyphs `first`, `second` and, if `withThird`,
// `third`: how many of them, and which glyph into `stands`. 0 for none.
std::size_t PlainLigature(const Bytes& image, std::size_t record, std::uint32_t first, std::uint32_t second,
                          bool withThird, std::uint32_t third, std::uint32_t& stands) {
  const std::size_t count = image[record + 42];
  const std::size_t list = Get32(image, record + 44);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t at = list + (i * kLigatureBytes);
    const std::uint32_t wantsThird = Get16(image, at + 4);
    if (Get16(image, at) != first || Get16(image, at + 2) != second ||
        (wantsThird != 0 && (!withThird || wantsThird != third))) {
      continue;
    }
    // The first that fits is the one, whether or not its glyph can be had.
    stands = Get16(image, at + 6);
    if (!PlainGlyph(image, record, stands, nullptr)) {
      return 0;
    }
    return wantsThird != 0 ? 3 : 2;
  }
  return 0;
}

// Of every two and three of `glyphs`, which are the face's, it makes the
// ligature the image's bytes say; and none of a glyph that says it is
// another face's.
void CheckLigatures(const Bytes& image, const Face& face, std::size_t number, const std::vector<Glyph>& glyphs) {
  const std::size_t record = kHeaderBytes + (number * kFaceBytes);
  BN_FUZZ_CHECK(face.Ligatures() == image[record + 42], "a face's ligatures are not as many as the image says");
  // The marks of the glyphs that begin one, from the list read the plain
  // way.
  std::uint64_t firsts = 0;
  for (std::size_t i = 0; i < image[record + 42]; ++i) {
    firsts |= std::uint64_t{1} << (Get16(image, Get32(image, record + 44) + (i * kLigatureBytes)) % 64U);
  }
  BN_FUZZ_CHECK(face.LigatureFirsts() == firsts, "the glyphs that begin a ligature are not marked as the image says");
  for (const Glyph& first : glyphs) {
    for (const Glyph& second : glyphs) {
      const std::array<Glyph, 2> two = {first, second};
      Glyph ligature;
      std::uint32_t stands = 0;
      std::size_t wanted = PlainLigature(image, record, first.index, second.index, false, 0, stands);
      std::size_t made = face.Ligature(two, ligature);
      BN_FUZZ_CHECK(made == wanted && (made == 0 || ligature.index == stands),
                    "two glyphs are not joined as the image's bytes say");
      if (made != 0) {
        CheckGlyph(image, face, ligature);
      }
      Glyph stranger = second;
      stranger.face = static_cast<std::uint16_t>(second.face + 1U);
      const std::array<Glyph, 2> strange = {first, stranger};
      const std::array<Glyph, 2> stranged = {stranger, second};
      BN_FUZZ_CHECK(face.Ligature(strange, ligature) == 0 && face.Ligature(stranged, ligature) == 0,
                    "a glyph of another face is joined");
      for (const Glyph& third : glyphs) {
        const std::array<Glyph, 3> three = {first, second, third};
        wanted = PlainLigature(image, record, first.index, second.index, true, third.index, stands);
        made = face.Ligature(three, ligature);
        BN_FUZZ_CHECK(made == wanted && (made == 0 || ligature.index == stands),
                      "three glyphs are not joined as the image's bytes say");
      }
    }
  }
  const std::array<Glyph, 1> one = {glyphs.empty() ? Glyph{} : glyphs[0]};
  Glyph none;
  BN_FUZZ_CHECK(face.Ligature(one, none) == 0, "one glyph is joined");
}

void CheckFace(const Bytes& image, const FontImage& fonts, std::size_t number, std::span<const std::uint32_t> asked) {
  const Face face = fonts.FaceAt(number);
  BN_FUZZ_CHECK(face.Valid(), "a face the image has is not one");
  const std::size_t record = kHeaderBytes + (number * kFaceBytes);
  BN_FUZZ_CHECK(static_cast<unsigned>(face.Role()) <= 3 && face.Style() <= 3, "a role or a style there is none of");
  BN_FUZZ_CHECK(face.Size() == Get16(image, record + 2) && face.LineHeight() == Get16(image, record + 8) &&
                    face.XHeight() == Get16(image, record + 10) && face.Glyphs() == Get32(image, record + 20),
                "a face's numbers are not the image's");
  static_cast<void>(face.Ascent());
  static_cast<void>(face.Descent());

  // It is found by what it is: the first such face, which is like it.
  const Face found = fonts.Find(face.Role(), face.Style(), face.Size());
  BN_FUZZ_CHECK(
      found.Valid() && found.Role() == face.Role() && found.Style() == face.Style() && found.Size() == face.Size(),
      "a face is not found by its role, style and size");
  BN_FUZZ_CHECK(fonts.Nearest(face.Role(), face.Style(), face.Size()).Valid(), "a face is not the nearest to itself");
  // The nearest to each style, found the plain way.
  for (std::uint8_t style = 0; style <= 3; ++style) {
    const std::array<std::uint8_t, 4> order = {style, static_cast<std::uint8_t>(style & 1U),
                                               static_cast<std::uint8_t>(style & 2U), 0};
    int wanted = -1;
    for (const std::uint8_t tried : order) {
      for (std::size_t other = 0; other < fonts.Faces() && wanted < 0; ++other) {
        const std::size_t at = kHeaderBytes + (other * kFaceBytes);
        if (image[at] == static_cast<std::uint8_t>(face.Role()) && image[at + 1] == tried &&
            Get16(image, at + 2) == face.Size()) {
          wanted = tried;
        }
      }
    }
    const Face nearest = fonts.Nearest(face.Role(), style, face.Size());
    BN_FUZZ_CHECK(nearest.Valid() == (wanted >= 0), "a nearest face is given, or is not, against what the faces are");
    BN_FUZZ_CHECK(!nearest.Valid() ||
                      (nearest.Style() == wanted && nearest.Role() == face.Role() && nearest.Size() == face.Size()),
                  "the nearest face is not of the first style the image has of those the style gives way to");
  }

  const std::vector<Range> ranges = RangesOf(image, record);
  const bool inOrder = InOrder(ranges);
  const auto mostSteps = static_cast<std::uint32_t>(std::bit_width(ranges.size()));

  // The code points asked for, and those at the edges of each range.
  std::vector<std::uint32_t> codePoints(asked.begin(), asked.end());
  for (const Range& range : ranges) {
    codePoints.push_back(range.first);
    codePoints.push_back(range.first - 1);
    codePoints.push_back(range.first + range.count - 1);
    codePoints.push_back(range.first + range.count);
  }
  std::vector<Glyph> foundGlyphs;
  foundGlyphs.reserve(kGlyphsKerned);
  for (const std::uint32_t codePoint : codePoints) {
    Glyph glyph;
    const bool has = face.Find(codePoint, glyph);
    BN_FUZZ_CHECK(face.LastSteps() <= mostSteps, "finding a glyph took more steps than halving does");
    BN_FUZZ_CHECK((face.LastSteps() == 0) == ranges.empty(), "the steps of finding a glyph are not counted");
    BN_FUZZ_CHECK(face.LastSteps() <= Face::kMostSteps, "finding a glyph took more steps than it says it may");
    if (has) {
      CheckGlyph(image, face, glyph);
      if (foundGlyphs.size() < kGlyphsKerned) {
        foundGlyphs.push_back(glyph);
      }
    }
    if (!inOrder) {
      continue;
    }
    // One by one.
    bool wanted = false;
    for (const Range& range : ranges) {
      if (codePoint >= range.first && codePoint - range.first < range.count) {
        wanted =
            PlainGlyph(image, record, std::uint64_t{range.glyph} + (codePoint - range.first), has ? &glyph : nullptr);
        break;
      }
    }
    BN_FUZZ_CHECK(has == wanted, "a code point has a glyph, or has none, against what its ranges say");
  }

  // By number: the first, the last, and the one past it.
  for (const std::uint32_t index : {std::uint32_t{0}, face.Glyphs() - 1, face.Glyphs(), face.Glyphs() / 2}) {
    Glyph glyph;
    const bool has = face.GlyphAt(index, glyph);
    BN_FUZZ_CHECK(has == PlainGlyph(image, record, index, has ? &glyph : nullptr),
                  "a glyph is given by its number, or is not, against what the image's bytes say");
    if (has) {
      CheckGlyph(image, face, glyph);
    }
  }
  Glyph missing;
  Glyph first;
  BN_FUZZ_CHECK(face.Missing(missing) == face.GlyphAt(0, first), "the glyph for what a face lacks is not glyph 0");
  CheckKerning(image, face, number, foundGlyphs);
  CheckLigatures(image, face, number, foundGlyphs);
}

bool SameGlyph(const Glyph& one, const Glyph& other) {
  return one.index == other.index && one.advance == other.advance && one.width == other.width &&
         one.height == other.height && one.left == other.left && one.top == other.top &&
         one.pictureAt == other.pictureAt && std::ranges::equal(one.picture, other.picture) &&
         one.kernFirst == other.kernFirst && one.kernSecond == other.kernSecond && one.face == other.face;
}

// The same image through a map, beside `whole`, which has it opened as a
// span. With `room`, the map has that many bytes of room and no more.
void CheckThroughAMap(const Bytes& image, const FontImage& whole, std::span<const std::uint32_t> asked,
                      std::size_t room) {
  bn::test::CopyingByteMap map(image, room);
  {
    FontImage fonts;
    const FontStatus status = fonts.Open(map);
    const std::size_t head = kHeaderBytes + (whole.Faces() * kFaceBytes);
    if (status == FontStatus::kNoRoom) {
      BN_FUZZ_CHECK(room < head, "there was room for the table of faces, and it was not had");
      BN_FUZZ_CHECK(fonts.Faces() == 0 && map.Stretches() == 0, "an image that was refused has faces, or bytes mapped");
      return;
    }
    BN_FUZZ_CHECK(status == FontStatus::kOk, "an image that opens whole does not open through a map");
    BN_FUZZ_CHECK(fonts.Faces() == whole.Faces(), "not the faces the whole image has");
    BN_FUZZ_CHECK(map.Stretches() == 1 && map.MappedBytes() == head,
                  "more is mapped at first than the header and the table of faces");
    for (std::size_t number = 0; number < fonts.Faces() && number < kFacesTried; ++number) {
      const Face one = whole.FaceAt(number);
      Face other = fonts.FaceAt(number);
      BN_FUZZ_CHECK(other.Valid() && one.Role() == other.Role() && one.Style() == other.Style() &&
                        one.Size() == other.Size() && one.Ascent() == other.Ascent() &&
                        one.Descent() == other.Descent() && one.LineHeight() == other.LineHeight() &&
                        one.XHeight() == other.XHeight() && one.Glyphs() == other.Glyphs() &&
                        one.Ligatures() == other.Ligatures() && one.KernFirsts() == other.KernFirsts() &&
                        one.KernSeconds() == other.KernSeconds(),
                    "a face has other numbers through a map");
      Glyph glyph;
      Glyph same;
      BN_FUZZ_CHECK(!other.Held() && !other.Missing(glyph) && !other.Find(asked[0], glyph) && other.LastSteps() == 0,
                    "a face that is not held has a glyph");
      // Its stretch of the image, worked out here: from the first of its
      // tables to the end of the last, its kerning and its ligatures among
      // them if it has any.
      const std::size_t record = kHeaderBytes + (number * kFaceBytes);
      const std::uint64_t rangesAt = Get32(image, record + 16);
      const std::uint64_t glyphsAt = Get32(image, record + 24);
      const std::uint64_t picturesAt = Get32(image, record + 28);
      const std::uint64_t kerningAt = Get32(image, record + 36);
      const std::uint64_t kerningBytes = std::uint64_t{image[record + 40]} * image[record + 41];
      std::uint64_t begins = std::min({rangesAt, glyphsAt, picturesAt});
      std::uint64_t ends = std::max({rangesAt + (std::uint64_t{Get32(image, record + 12)} * kRangeBytes),
                                     glyphsAt + (std::uint64_t{Get32(image, record + 20)} * kGlyphBytes),
                                     picturesAt + Get32(image, record + 32)});
      if (kerningBytes > 0) {
        begins = std::min(begins, kerningAt);
        ends = std::max(ends, kerningAt + kerningBytes);
      }
      const std::uint64_t ligaturesAt = Get32(image, record + 44);
      const std::uint64_t ligatureBytes = std::uint64_t{image[record + 42]} * kLigatureBytes;
      if (ligatureBytes > 0) {
        begins = std::min(begins, ligaturesAt);
        ends = std::max(ends, ligaturesAt + ligatureBytes);
      }
      const bool held = fonts.Hold(other);
      BN_FUZZ_CHECK(held == (ends - begins <= room - head),
                    "a face is held, or is not, against the room there is for its stretch");
      BN_FUZZ_CHECK(other.Held() == held && fonts.Held() == (held ? 1U : 0U), "a face is not held as Hold() said");
      if (!held) {
        BN_FUZZ_CHECK(map.Stretches() == 1 && !other.Missing(glyph), "a face that was refused left something behind");
        continue;
      }
      BN_FUZZ_CHECK(fonts.HeldBytes() == ends - begins && map.MappedBytes() == head + (ends - begins),
                    "what is mapped for a face is not its own stretch");
      BN_FUZZ_CHECK(fonts.FaceAt(number).Held() && fonts.Hold(other) && fonts.Held() == 1,
                    "a face held is not held when found or asked again");
      // What the whole image's face gives, it gives: read from its stretch
      // alone, which is a copy of exactly its size.
      std::vector<std::uint32_t> codePoints(asked.begin(), asked.end());
      for (const Range& range : RangesOf(image, record)) {
        codePoints.push_back(range.first);
        codePoints.push_back(range.first + range.count - 1);
      }
      std::vector<Glyph> wholeGlyphs;
      std::vector<Glyph> mappedGlyphs;
      for (const std::uint32_t codePoint : codePoints) {
        const bool found = one.Find(codePoint, glyph);
        BN_FUZZ_CHECK(other.Find(codePoint, same) == found && one.LastSteps() == other.LastSteps(),
                      "a code point is found through a map and not whole, or the other way");
        BN_FUZZ_CHECK(!found || SameGlyph(glyph, same), "a glyph is another through a map");
        if (found && wholeGlyphs.size() < kGlyphsKerned) {
          wholeGlyphs.push_back(glyph);
          mappedGlyphs.push_back(same);
        }
        if (found) {
          const std::span<const std::uint8_t> bytes = fonts.Bytes(same.pictureAt, same.picture.size());
          BN_FUZZ_CHECK(same.picture.empty() ? bytes.empty() : bytes.data() == same.picture.data(),
                        "a picture is not to be had again from where the glyph said it is");
        }
      }
      for (const std::uint32_t index : {std::uint32_t{0}, one.Glyphs() - 1, one.Glyphs(), one.Glyphs() / 2}) {
        const bool found = one.GlyphAt(index, glyph);
        BN_FUZZ_CHECK(other.GlyphAt(index, same) == found && (!found || SameGlyph(glyph, same)),
                      "a glyph by its number is another through a map");
      }
      // Its kerning, read from its stretch, is the whole image's.
      for (std::size_t first = 0; first < wholeGlyphs.size(); ++first) {
        for (std::size_t second = 0; second < wholeGlyphs.size(); ++second) {
          BN_FUZZ_CHECK(other.Kern(mappedGlyphs[first], mappedGlyphs[second]) ==
                            one.Kern(wholeGlyphs[first], wholeGlyphs[second]),
                        "two glyphs are kerned otherwise through a map");
        }
      }
      // And its ligatures.
      for (std::size_t first = 0; first < wholeGlyphs.size(); ++first) {
        for (std::size_t second = 0; second < wholeGlyphs.size(); ++second) {
          for (std::size_t third = 0; third < wholeGlyphs.size(); ++third) {
            const std::array<Glyph, 3> wholeThree = {wholeGlyphs[first], wholeGlyphs[second], wholeGlyphs[third]};
            const std::array<Glyph, 3> mappedThree = {mappedGlyphs[first], mappedGlyphs[second], mappedGlyphs[third]};
            Glyph wholeLigature;
            Glyph mappedLigature;
            const std::size_t made = one.Ligature(wholeThree, wholeLigature);
            BN_FUZZ_CHECK(other.Ligature(mappedThree, mappedLigature) == made &&
                              (made == 0 || SameGlyph(wholeLigature, mappedLigature)),
                          "three glyphs are joined otherwise through a map");
          }
        }
      }
      fonts.ReleaseFaces();
      BN_FUZZ_CHECK(fonts.Held() == 0 && map.Stretches() == 1 && !fonts.FaceAt(number).Held(),
                    "a face given up is still held");
      BN_FUZZ_CHECK(wholeGlyphs.empty() || fonts.FaceAt(number).Kern(wholeGlyphs[0], wholeGlyphs[0]) == 0,
                    "a face given up kerns");
      Glyph joined;
      BN_FUZZ_CHECK(wholeGlyphs.size() < 2 || fonts.FaceAt(number).Ligature(wholeGlyphs, joined) == 0,
                    "a face given up joins");
    }
  }
  BN_FUZZ_CHECK(map.Stretches() == 0 && map.MappedBytes() == 0, "bytes are left mapped when the image is gone");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  std::vector<std::uint32_t> asked;
  asked.reserve(4);
  for (int i = 0; i < 4; ++i) {
    asked.push_back(input.Number16());
  }
  const std::span<const std::uint8_t> rest = input.Rest();
  Bytes image(rest.begin(), rest.end());
  if ((choices & 1U) != 0 && image.size() >= kHeaderBytes) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      image[8 + byte] = static_cast<std::uint8_t>(image.size() >> (8U * byte));
    }
  }

  FontImage fonts;
  const FontStatus status = fonts.Open(image);
  BN_FUZZ_CHECK(status == fonts.Status(), "the status given is not the one kept");
  FontImage again;
  BN_FUZZ_CHECK(again.Open(image) == status && again.Faces() == fonts.Faces(), "the same bytes opened another way");
  if (status != FontStatus::kOk) {
    BN_FUZZ_CHECK(status == FontStatus::kBadImage, "an image was refused for no reason there is");
    BN_FUZZ_CHECK(fonts.Faces() == 0 && !fonts.FaceAt(0).Valid(), "an image that was refused has faces");
    // And is refused through a map that has room for all of it.
    bn::test::CopyingByteMap map(image);
    FontImage mapped;
    BN_FUZZ_CHECK(mapped.Open(map) == FontStatus::kBadImage && mapped.Faces() == 0 && map.Stretches() == 0,
                  "an image refused as a span is not refused through a map, or is left mapped");
    return 0;
  }
  BN_FUZZ_CHECK(fonts.Faces() <= FontImage::kMostFaces, "more faces than an image may have");
  BN_FUZZ_CHECK(fonts.Faces() == Get16(image, 6), "not as many faces as the image says");
  BN_FUZZ_CHECK(!fonts.FaceAt(fonts.Faces()).Valid(), "a face past the last");
  for (std::size_t number = 0; number < fonts.Faces() && number < kFacesTried; ++number) {
    CheckFace(image, fonts, number, asked);
  }
  if (fonts.Faces() > kFacesTried) {
    CheckFace(image, fonts, fonts.Faces() - 1, asked);
  }
  // Through a map: with room for all of it, or for a part the input says.
  std::size_t room = image.size() * 2;
  if ((choices & 2U) != 0) {
    room = (image.size() * (1 + ((choices >> 2U) & 7U))) / 8;
  }
  CheckThroughAMap(image, fonts, asked, room);
  return 0;
}
