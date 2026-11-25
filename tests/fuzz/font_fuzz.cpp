// The fuzz target for the reader of the fonts image (draw/font.h).
//
// What it is given:
//
//   byte 0        bit 0: the image is made to say it is as long as it is,
//                 so that tables near its end are in it
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
//     image's bytes say, read here the plain way.
//
// The image is in memory of exactly its size, so the sanitizer sees a byte
// read outside it.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
  for (const std::uint32_t codePoint : codePoints) {
    Glyph glyph;
    const bool has = face.Find(codePoint, glyph);
    BN_FUZZ_CHECK(face.LastSteps() <= mostSteps, "finding a glyph took more steps than halving does");
    BN_FUZZ_CHECK((face.LastSteps() == 0) == ranges.empty(), "the steps of finding a glyph are not counted");
    BN_FUZZ_CHECK(face.LastSteps() <= Face::kMostSteps, "finding a glyph took more steps than it says it may");
    if (has) {
      CheckGlyph(image, face, glyph);
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
  return 0;
}
