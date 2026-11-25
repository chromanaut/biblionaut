#include "draw/font.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "byte_map_fakes.h"
#include "core/byte_map.h"
#include "font_image.h"

namespace {

using bn::draw::Face;
using bn::draw::FaceRole;
using bn::draw::FontImage;
using bn::draw::FontStatus;
using bn::draw::Glyph;
using bn::draw::kBold;
using bn::draw::kItalic;
using bn::test::Block;
using bn::test::FontImageBytes;
using bn::test::ImageFace;
using bn::test::ImageFacePlaces;
using bn::test::ImageGlyph;
using bn::test::PutLe16;
using bn::test::PutLe32;

// A face with the letters a to e, a space, and é.
ImageFace SmallFace(std::uint8_t role = 0, std::uint8_t style = 0, std::uint16_t size = 20) {
  ImageFace face = bn::test::EmptyFace(role, style, size);
  face.glyphs[' '] = bn::test::Blank(20);
  for (std::uint32_t letter = 'a'; letter <= 'e'; ++letter) {
    face.glyphs[letter] = Block(static_cast<std::uint16_t>(40 + letter - 'a'), 9, 10);
  }
  face.glyphs[0xE9] = Block(44, 9, 14, 2);
  return face;
}

// Asks an opened image for everything it can be asked for, reading every
// byte it gives; with the image in memory of exactly its size, the
// sanitizer sees a byte read outside it. True if all it gave lay inside.
bool EverythingLiesInside(const FontImage& fonts, std::span<const std::uint8_t> image) {
  const auto inside = [&](const Glyph& glyph) {
    if (glyph.picture.size() != bn::draw::PictureBytes(glyph.width, glyph.height)) {
      return false;
    }
    if (!glyph.picture.empty() && (glyph.picture.data() < image.data() ||
                                   glyph.picture.data() + glyph.picture.size() > image.data() + image.size())) {
      return false;
    }
    std::uint32_t ink = 0;
    for (std::uint32_t y = 0; y < glyph.height; ++y) {
      for (std::uint32_t x = 0; x < glyph.width; ++x) {
        ink += glyph.Ink(x, y);
      }
    }
    return ink <= 3U * glyph.width * glyph.height;
  };
  for (std::size_t i = 0; i < fonts.Faces(); ++i) {
    const Face face = fonts.FaceAt(i);
    if (!face.Valid()) {
      return false;
    }
    static_cast<void>(face.Role());
    static_cast<void>(face.Style());
    static_cast<void>(face.Size());
    static_cast<void>(face.Ascent());
    static_cast<void>(face.Descent());
    static_cast<void>(face.LineHeight());
    static_cast<void>(face.XHeight());
    Glyph glyph;
    for (std::uint32_t codePoint = 0; codePoint < 0x200; ++codePoint) {
      if (face.Find(codePoint, glyph) && !inside(glyph)) {
        return false;
      }
      if (face.LastSteps() > Face::kMostSteps) {
        return false;
      }
    }
    for (std::uint32_t index = 0; index < face.Glyphs() && index < 64; ++index) {
      if (face.GlyphAt(index, glyph) && !inside(glyph)) {
        return false;
      }
    }
    if (face.Missing(glyph) && !inside(glyph)) {
      return false;
    }
  }
  return true;
}

TEST(FontImage, IsNotOpenUntilItIsOpened) {
  const FontImage fonts;
  EXPECT_EQ(fonts.Status(), FontStatus::kNotOpen);
  EXPECT_EQ(fonts.Faces(), 0U);
  EXPECT_FALSE(fonts.FaceAt(0).Valid());
  EXPECT_FALSE(fonts.Find(FaceRole::kSerif, 0, 20).Valid());
  EXPECT_STREQ(bn::draw::FontStatusName(FontStatus::kNotOpen), "not open");
  EXPECT_STREQ(bn::draw::FontStatusName(FontStatus::kOk), "ok");
  EXPECT_STREQ(bn::draw::FontStatusName(FontStatus::kBadImage), "bad image");
}

TEST(FontImage, GivesEachFaceWithItsNumbers) {
  ImageFace serif = SmallFace(0, 0, 20);
  serif.ascent = 17;
  serif.descent = 6;
  serif.lineHeight = 25;
  serif.xHeight = 11;
  const std::vector<std::uint8_t> image =
      FontImageBytes({serif, SmallFace(1, kBold | kItalic, 26), SmallFace(3, 0, 20)});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  ASSERT_EQ(fonts.Faces(), 3U);

  const Face first = fonts.FaceAt(0);
  ASSERT_TRUE(first.Valid());
  EXPECT_EQ(first.Role(), FaceRole::kSerif);
  EXPECT_EQ(first.Style(), 0);
  EXPECT_EQ(first.Size(), 20);
  EXPECT_EQ(first.Ascent(), 17);
  EXPECT_EQ(first.Descent(), 6);
  EXPECT_EQ(first.LineHeight(), 25);
  EXPECT_EQ(first.XHeight(), 11);
  EXPECT_EQ(first.Glyphs(), 8U);  // Glyph 0, a space, five letters and é.

  const Face second = fonts.FaceAt(1);
  EXPECT_EQ(second.Role(), FaceRole::kSans);
  EXPECT_EQ(second.Style(), kBold | kItalic);
  EXPECT_EQ(second.Size(), 26);
  EXPECT_EQ(fonts.FaceAt(2).Role(), FaceRole::kFallback);
  EXPECT_FALSE(fonts.FaceAt(3).Valid());
}

TEST(FontImage, FindsAFaceByItsRoleStyleAndSize) {
  const std::vector<std::uint8_t> image =
      FontImageBytes({SmallFace(0, 0, 20), SmallFace(0, kItalic, 20), SmallFace(0, 0, 26), SmallFace(1, 0, 20),
                      SmallFace(2, kBold, 20)});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);

  const Face italic = fonts.Find(FaceRole::kSerif, kItalic, 20);
  ASSERT_TRUE(italic.Valid());
  EXPECT_EQ(italic.Style(), kItalic);
  EXPECT_EQ(fonts.Find(FaceRole::kSerif, 0, 26).Size(), 26);
  EXPECT_EQ(fonts.Find(FaceRole::kMono, kBold, 20).Role(), FaceRole::kMono);
  EXPECT_FALSE(fonts.Find(FaceRole::kSerif, kBold, 20).Valid());
  EXPECT_FALSE(fonts.Find(FaceRole::kSerif, 0, 21).Valid());
  EXPECT_FALSE(fonts.Find(FaceRole::kFallback, 0, 20).Valid());
}

TEST(FontImage, GivesTheNearestStyleARoleHas) {
  // A serif with all four, a sans with a regular and a bold, a monospace
  // with an italic alone, and no fallback. At 26, a serif with all but the
  // bold italic, and a sans with a regular alone.
  const std::vector<std::uint8_t> image =
      FontImageBytes({SmallFace(0, 0), SmallFace(0, kBold), SmallFace(0, kItalic), SmallFace(0, kBold | kItalic),
                      SmallFace(1, 0), SmallFace(1, kBold), SmallFace(2, kItalic), SmallFace(0, kItalic, 26),
                      SmallFace(0, kBold, 26), SmallFace(0, 0, 26), SmallFace(1, 0, 26)});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);

  EXPECT_EQ(fonts.Nearest(FaceRole::kSerif, kBold | kItalic, 20).Style(), kBold | kItalic);
  EXPECT_EQ(fonts.Nearest(FaceRole::kSerif, kItalic, 20).Style(), kItalic);
  // Bold italic gives way to bold, and italic to regular.
  EXPECT_EQ(fonts.Nearest(FaceRole::kSans, kBold | kItalic, 20).Style(), kBold);
  EXPECT_EQ(fonts.Nearest(FaceRole::kSans, kItalic, 20).Style(), 0);
  EXPECT_TRUE(fonts.Nearest(FaceRole::kSans, kItalic, 20).Valid());
  // Bold italic to italic, where there is no bold.
  EXPECT_EQ(fonts.Nearest(FaceRole::kMono, kBold | kItalic, 20).Style(), kItalic);
  // To bold sooner than to italic, where there are both; and to regular,
  // where there is neither.
  EXPECT_EQ(fonts.Nearest(FaceRole::kSerif, kBold | kItalic, 26).Style(), kBold);
  EXPECT_EQ(fonts.Nearest(FaceRole::kSerif, kBold | kItalic, 26).Size(), 26);
  EXPECT_TRUE(fonts.Nearest(FaceRole::kSans, kBold | kItalic, 26).Valid());
  EXPECT_EQ(fonts.Nearest(FaceRole::kSans, kBold | kItalic, 26).Style(), 0);
  EXPECT_TRUE(fonts.Nearest(FaceRole::kSans, kBold, 26).Valid());
  // Nothing the role has at the size is near.
  EXPECT_FALSE(fonts.Nearest(FaceRole::kMono, kBold, 20).Valid());
  EXPECT_FALSE(fonts.Nearest(FaceRole::kFallback, 0, 20).Valid());
  EXPECT_FALSE(fonts.Nearest(FaceRole::kSerif, 0, 30).Valid());
}

TEST(Face, FindsAGlyphByItsCodePoint) {
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace()});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  const Face face = fonts.FaceAt(0);

  Glyph glyph;
  ASSERT_TRUE(face.Find('c', glyph));
  EXPECT_EQ(glyph.advance, 42);
  EXPECT_EQ(glyph.width, 9);
  EXPECT_EQ(glyph.height, 10);
  EXPECT_EQ(glyph.left, 0);
  EXPECT_EQ(glyph.top, 10);
  EXPECT_EQ(glyph.index, 4U);  // After glyph 0, the space, a and b.
  EXPECT_EQ(glyph.picture.size(), bn::draw::PictureBytes(9, 10));

  ASSERT_TRUE(face.Find(0xE9, glyph));
  EXPECT_EQ(glyph.advance, 44);
  EXPECT_EQ(glyph.height, 14);

  // A space has an advance and nothing to draw.
  ASSERT_TRUE(face.Find(' ', glyph));
  EXPECT_EQ(glyph.advance, 20);
  EXPECT_TRUE(glyph.picture.empty());

  for (const std::uint32_t lacking : {0U, 0x1FU, 0x21U, 0x60U, 0x66U, 0xE8U, 0xEAU, 0x10FFFFU, 0xFFFFFFFFU}) {
    EXPECT_FALSE(face.Find(lacking, glyph)) << lacking;
  }
}

TEST(Face, GivesAPicturesInkAPixelAtATime) {
  ImageFace face;
  // Three by three, the levels going round, so that a picture's rows do not
  // end where its bytes do.
  face.glyphs['x'] =
      ImageGlyph{.advance = 12, .width = 3, .height = 3, .left = -1, .top = 3, .ink = {0, 1, 2, 3, 0, 1, 2, 3, 3}};
  face.glyphs['y'] = Block(8, 2, 2, 3);
  const std::vector<std::uint8_t> image = FontImageBytes({face});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);

  Glyph glyph;
  ASSERT_TRUE(fonts.FaceAt(0).Find('x', glyph));
  EXPECT_EQ(glyph.left, -1);
  ASSERT_EQ(glyph.picture.size(), 3U);  // 18 bits.
  const std::vector<std::uint8_t> wanted = {0, 1, 2, 3, 0, 1, 2, 3, 3};
  for (std::uint32_t y = 0; y < 3; ++y) {
    for (std::uint32_t x = 0; x < 3; ++x) {
      EXPECT_EQ(glyph.Ink(x, y), wanted[(y * 3) + x]) << x << "," << y;
    }
  }
  // The glyph after it begins at its own byte.
  ASSERT_TRUE(fonts.FaceAt(0).Find('y', glyph));
  EXPECT_EQ(glyph.Ink(0, 0), 3);
  EXPECT_EQ(glyph.Ink(1, 1), 3);
}

TEST(Face, DrawsGlyphZeroForWhatItLacks) {
  ImageFace made = SmallFace();
  made.missing = Block(36, 7, 11, 1);
  const std::vector<std::uint8_t> image = FontImageBytes({made});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  const Face face = fonts.FaceAt(0);

  Glyph glyph;
  EXPECT_FALSE(face.Find('z', glyph));
  ASSERT_TRUE(face.Missing(glyph));
  EXPECT_EQ(glyph.index, 0U);
  EXPECT_EQ(glyph.advance, 36);
  EXPECT_EQ(glyph.width, 7);
  EXPECT_EQ(glyph.Ink(3, 5), 1);

  ASSERT_TRUE(face.GlyphAt(7, glyph));  // é, the last.
  EXPECT_EQ(glyph.advance, 44);
  EXPECT_FALSE(face.GlyphAt(8, glyph));
  EXPECT_FALSE(face.GlyphAt(0xFFFFFFFFU, glyph));
}

TEST(Face, ThatIsNotOneHasNothing) {
  const Face face;
  EXPECT_FALSE(face.Valid());
  EXPECT_EQ(face.Size(), 0);
  EXPECT_EQ(face.Ascent(), 0);
  EXPECT_EQ(face.Descent(), 0);
  EXPECT_EQ(face.LineHeight(), 0);
  EXPECT_EQ(face.XHeight(), 0);
  EXPECT_EQ(face.Glyphs(), 0U);
  Glyph glyph;
  EXPECT_FALSE(face.Find('a', glyph));
  EXPECT_EQ(face.LastSteps(), 0U);
  EXPECT_FALSE(face.GlyphAt(0, glyph));
  EXPECT_FALSE(face.Missing(glyph));
}

TEST(Face, FindsAGlyphInNoMoreStepsThanHalvingTakes) {
  // Every other code point, so that each is a range of its own.
  ImageFace made;
  constexpr std::uint32_t kRanges = 5000;
  for (std::uint32_t i = 0; i < kRanges; ++i) {
    made.glyphs[0x100 + (i * 2)] = bn::test::Blank(static_cast<std::uint16_t>(i));
  }
  const std::vector<std::uint8_t> image = FontImageBytes({made});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  const Face face = fonts.FaceAt(0);

  Glyph glyph;
  std::uint32_t most = 0;
  std::uint32_t fewest = Face::kMostSteps;
  for (std::uint32_t i = 0; i < kRanges; ++i) {
    ASSERT_TRUE(face.Find(0x100 + (i * 2), glyph)) << i;
    ASSERT_EQ(glyph.advance, i);
    ASSERT_LE(face.LastSteps(), 13U);  // 5,000 is less than 2^13.
    most = std::max(most, face.LastSteps());
    fewest = std::min(fewest, face.LastSteps());
    ASSERT_FALSE(face.Find(0x101 + (i * 2), glyph)) << i;
    // What is not there is only known not to be when the halving is done.
    ASSERT_GE(face.LastSteps(), 12U);
    ASSERT_LE(face.LastSteps(), 13U);
  }
  // The steps are counted: the middle range is found at the first, and some
  // range takes all thirteen.
  EXPECT_EQ(fewest, 1U);
  EXPECT_EQ(most, 13U);
  EXPECT_FALSE(face.Find(0xFF, glyph));
  EXPECT_GE(face.LastSteps(), 12U);
  EXPECT_LE(face.LastSteps(), 13U);
  EXPECT_LE(13U, Face::kMostSteps);
}

TEST(Face, FindsNothingThroughARangeThatPointsPastItsGlyphs) {
  std::vector<ImageFacePlaces> places;
  std::vector<std::uint8_t> image = FontImageBytes({SmallFace()}, &places);
  // The range of a to e: its first glyph made the last but one, so that c
  // and after are glyphs the face does not have; then made as large as a
  // number can be, so that adding to it goes past what 32 bits hold.
  const std::size_t range = places[0].ranges + 12;
  PutLe32(image, range + 8, 6);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  Glyph glyph;
  EXPECT_TRUE(fonts.FaceAt(0).Find('a', glyph));
  EXPECT_TRUE(fonts.FaceAt(0).Find('b', glyph));
  EXPECT_FALSE(fonts.FaceAt(0).Find('c', glyph));
  EXPECT_FALSE(fonts.FaceAt(0).Find('e', glyph));

  PutLe32(image, range + 8, 0xFFFFFFFFU);
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  EXPECT_FALSE(fonts.FaceAt(0).Find('a', glyph));
  EXPECT_FALSE(fonts.FaceAt(0).Find('b', glyph));
  EXPECT_TRUE(EverythingLiesInside(fonts, image));
}

TEST(Face, GivesNoGlyphWhosePictureLiesOutsideItsPictures) {
  std::vector<ImageFacePlaces> places;
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace(), SmallFace(1)}, &places);
  const std::size_t record = places[0].glyphs + 48;  // The letter c: glyph 4, of 12 bytes each.
  FontImage fonts;
  Glyph glyph;

  // Begun a byte too late to fit in.
  std::vector<std::uint8_t> image = good;
  PutLe32(image, record + 8, static_cast<std::uint32_t>(places[0].pictureBytes - bn::draw::PictureBytes(9, 10) + 1));
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  EXPECT_FALSE(fonts.FaceAt(0).Find('c', glyph));
  EXPECT_TRUE(fonts.FaceAt(0).Find('b', glyph));
  EXPECT_TRUE(EverythingLiesInside(fonts, image));

  // Begun far outside.
  image = good;
  PutLe32(image, record + 8, 0xFFFFFFF0U);
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  EXPECT_FALSE(fonts.FaceAt(0).Find('c', glyph));

  // Made larger than the face's pictures altogether.
  image = good;
  image[record + 2] = 255;
  image[record + 3] = 255;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  EXPECT_FALSE(fonts.FaceAt(0).Find('c', glyph));
  EXPECT_TRUE(EverythingLiesInside(fonts, image));

  // The last glyph of the first face, as it is, ends where its pictures do.
  ASSERT_EQ(fonts.Open(good), FontStatus::kOk);
  ASSERT_TRUE(fonts.FaceAt(0).Find(0xE9, glyph));
  EXPECT_EQ(glyph.picture.data() + glyph.picture.size(), good.data() + places[0].pictures + places[0].pictureBytes);
}

TEST(FontImage, RefusesWhatIsNotAnImage) {
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace()});
  FontImage fonts;
  ASSERT_EQ(fonts.Open(good), FontStatus::kOk);

  EXPECT_EQ(fonts.Open({}), FontStatus::kBadImage);
  EXPECT_EQ(fonts.Faces(), 0U);
  EXPECT_FALSE(fonts.FaceAt(0).Valid());

  for (std::size_t letter = 0; letter < 4; ++letter) {
    std::vector<std::uint8_t> image = good;
    image[letter] ^= 0x20U;
    EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage) << letter;
  }
  std::vector<std::uint8_t> image = good;
  image[4] = 2;  // A version this does not read.
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);
  image[4] = 0;
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);

  // Says it is longer than it is.
  image = good;
  PutLe32(image, 8, static_cast<std::uint32_t>(good.size() + 1));
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);

  // Says it has more faces than it has room for, or than an image may.
  image = good;
  PutLe16(image, 6, 400);
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);
  EXPECT_EQ(fonts.Status(), FontStatus::kBadImage);
}

TEST(FontImage, HoldsAsManyFacesAsItSaysItMayAndNoMore) {
  // Faces with no glyphs at all, glyph 0 included: a record each and
  // nothing else, so that nothing but their number is wrong with one too
  // many.
  const auto empty = [](std::size_t faces) {
    std::vector<std::uint8_t> image(16 + (faces * 48), 0);
    image[0] = 'B';
    image[1] = 'N';
    image[2] = 'F';
    image[3] = 'N';
    image[4] = 1;
    PutLe16(image, 6, static_cast<std::uint32_t>(faces));
    PutLe32(image, 8, static_cast<std::uint32_t>(image.size()));
    for (std::size_t face = 0; face < faces; ++face) {
      PutLe16(image, 16 + (face * 48) + 2, static_cast<std::uint32_t>(face + 1));  // Each its own size.
    }
    return image;
  };
  FontImage fonts;
  const std::vector<std::uint8_t> most = empty(FontImage::kMostFaces);
  ASSERT_EQ(fonts.Open(most), FontStatus::kOk);
  EXPECT_EQ(fonts.Faces(), FontImage::kMostFaces);
  const Face last = fonts.Find(FaceRole::kSerif, 0, 1024);
  ASSERT_TRUE(last.Valid());
  EXPECT_EQ(last.Glyphs(), 0U);
  Glyph glyph;
  EXPECT_FALSE(last.Missing(glyph));
  EXPECT_FALSE(last.Find('a', glyph));
  EXPECT_EQ(last.LastSteps(), 0U);
  EXPECT_TRUE(EverythingLiesInside(fonts, most));

  const std::vector<std::uint8_t> tooMany = empty(FontImage::kMostFaces + 1);
  EXPECT_EQ(fonts.Open(tooMany), FontStatus::kBadImage);
  EXPECT_EQ(fonts.Faces(), 0U);
}

TEST(FontImage, ReadsOnlyAsMuchAsItSaysItIs) {
  // An image with more after it, as a partition has after the image
  // written to it.
  std::vector<std::uint8_t> image = FontImageBytes({SmallFace()});
  const std::size_t size = image.size();
  image.resize(size + 4096, 0xFF);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(image), FontStatus::kOk);
  Glyph glyph;
  EXPECT_TRUE(fonts.FaceAt(0).Find('a', glyph));

  // A table that lies in what comes after is outside the image.
  PutLe32(image, 16 + 28, static_cast<std::uint32_t>(size + 8));
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);

  // An image with no faces is one.
  std::vector<std::uint8_t> none = FontImageBytes({});
  ASSERT_EQ(fonts.Open(none), FontStatus::kOk);
  EXPECT_EQ(fonts.Faces(), 0U);
  EXPECT_FALSE(fonts.Nearest(FaceRole::kSerif, 0, 20).Valid());
}

TEST(FontImage, RefusesAFaceWhoseTablesLieOutsideIt) {
  std::vector<ImageFacePlaces> places;
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace(), SmallFace(1)}, &places);
  const std::size_t record = places[1].record;
  const auto size = static_cast<std::uint32_t>(good.size());
  FontImage fonts;

  struct Spoiling {
    std::size_t at;  // In the record.
    std::uint32_t value;
  };
  for (const Spoiling& spoiling : {
           Spoiling{12, 0xFFFFFFFFU},                                             // More ranges than fit,
           Spoiling{12, (size / 12) + 1},                                         // by a few,
           Spoiling{16, size - 12},                                               // or begun too near the end,
           Spoiling{16, 0xFFFFFFF0U},                                             // or beyond it.
           Spoiling{20, 0x20000000U},                                             // The same of the glyphs,
           Spoiling{24, size - 24},                                               //
           Spoiling{24, size + 1},                                                //
           Spoiling{28, size - 8},                                                // and of the pictures.
           Spoiling{32, static_cast<std::uint32_t>(places[1].pictureBytes + 1)},  // Which end where the image does.
           Spoiling{32, 0xFFFFFFFFU},
       }) {
    std::vector<std::uint8_t> image = good;
    PutLe32(image, record + spoiling.at, spoiling.value);
    EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage) << spoiling.at << " " << spoiling.value;
    EXPECT_EQ(fonts.Faces(), 0U);
  }
  // A role or a style there is none of.
  std::vector<std::uint8_t> image = good;
  image[record] = 4;
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);
  image = good;
  image[record + 1] = 4;
  EXPECT_EQ(fonts.Open(image), FontStatus::kBadImage);
  EXPECT_EQ(fonts.Open(good), FontStatus::kOk);
}

TEST(FontImage, CutShortAnywhereIsRefused) {
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace(), SmallFace(2, kBold, 30)});
  FontImage fonts;
  for (std::size_t length = 0; length < good.size(); ++length) {
    // In memory of its own, so that a byte read past the cut is seen.
    const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    ASSERT_EQ(fonts.Open(cut), FontStatus::kBadImage) << length;
  }
}

TEST(FontImage, WithAnyByteChangedGivesNothingFromOutsideIt) {
  ImageFace many = SmallFace(1, kItalic, 24);
  for (std::uint32_t i = 0; i < 40; ++i) {
    many.glyphs[0x100 + (i * 3)] = Block(static_cast<std::uint16_t>(30 + i), 5, 6, 2);
  }
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace(), many});
  FontImage fonts;
  std::size_t opened = 0;
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      std::vector<std::uint8_t> image = good;
      image[at] ^= change;
      if (fonts.Open(image) == FontStatus::kOk) {
        ++opened;
        ASSERT_TRUE(EverythingLiesInside(fonts, image)) << at << " " << static_cast<int>(change);
      } else {
        ASSERT_EQ(fonts.Faces(), 0U);
      }
    }
  }
  // Most changes are to glyphs and pictures, which opening does not look at.
  EXPECT_GT(opened, good.size());
}

// ---- An image opened through a map ----

bool SameGlyph(const Glyph& one, const Glyph& other) {
  return one.index == other.index && one.advance == other.advance && one.width == other.width &&
         one.height == other.height && one.left == other.left && one.top == other.top &&
         one.pictureAt == other.pictureAt && std::ranges::equal(one.picture, other.picture);
}

// Whether `image` opened through a map goes as it goes opened whole; and,
// where it opens, whether every face has the same numbers, has no glyphs
// until it is held, and then gives for every code point and glyph asked
// what the whole image's face gives, the picture where the image has it.
// Each stretch mapped is a copy of exactly its size, so the sanitizer sees
// a byte read outside it; and nothing may be left mapped at the end.
::testing::AssertionResult SameThroughAMap(const std::vector<std::uint8_t>& image) {
  FontImage whole;
  const FontStatus status = whole.Open(image);
  bn::test::CopyingByteMap map(image);
  {
    FontImage mapped;
    if (mapped.Open(map) != status) {
      return ::testing::AssertionFailure() << "opened whole it is " << bn::draw::FontStatusName(status)
                                           << ", and through a map " << bn::draw::FontStatusName(mapped.Status());
    }
    if (status != FontStatus::kOk) {
      if (mapped.Faces() != 0 || map.Stretches() != 0) {
        return ::testing::AssertionFailure() << "an image that was refused has faces, or bytes mapped";
      }
      return ::testing::AssertionSuccess();
    }
    if (mapped.Faces() != whole.Faces() || map.Stretches() != 1) {
      return ::testing::AssertionFailure() << "the faces differ, or more is mapped than the table of them";
    }
    for (std::size_t i = 0; i < whole.Faces(); ++i) {
      const Face one = whole.FaceAt(i);
      Face other = mapped.FaceAt(i);
      if (!other.Valid() || one.Role() != other.Role() || one.Style() != other.Style() || one.Size() != other.Size() ||
          one.Ascent() != other.Ascent() || one.Descent() != other.Descent() ||
          one.LineHeight() != other.LineHeight() || one.XHeight() != other.XHeight() ||
          one.Glyphs() != other.Glyphs()) {
        return ::testing::AssertionFailure() << "face " << i << " has other numbers";
      }
      Glyph glyph;
      Glyph same;
      if (!one.Held() || other.Held() || other.Find('a', glyph) || other.Missing(glyph) || other.LastSteps() != 0) {
        return ::testing::AssertionFailure() << "face " << i << " has glyphs before it is held";
      }
      if (!mapped.Hold(other) || !other.Held() || mapped.Held() != 1) {
        return ::testing::AssertionFailure() << "face " << i << " could not be held";
      }
      for (std::uint32_t codePoint = 0; codePoint < 0x200; ++codePoint) {
        const bool found = one.Find(codePoint, glyph);
        if (other.Find(codePoint, same) != found || one.LastSteps() != other.LastSteps() ||
            (found && !SameGlyph(glyph, same))) {
          return ::testing::AssertionFailure() << "face " << i << " differs at code point " << codePoint;
        }
        // Where the image has the picture, and where it is to be read.
        if (found && !glyph.picture.empty() &&
            (glyph.pictureAt != static_cast<std::size_t>(glyph.picture.data() - image.data()) ||
             mapped.Bytes(same.pictureAt, same.picture.size()).data() != same.picture.data())) {
          return ::testing::AssertionFailure()
                 << "face " << i << " has a picture elsewhere, at code point " << codePoint;
        }
      }
      for (std::uint32_t index = 0; index < one.Glyphs() && index < 64; ++index) {
        const bool found = one.GlyphAt(index, glyph);
        if (other.GlyphAt(index, same) != found || (found && !SameGlyph(glyph, same))) {
          return ::testing::AssertionFailure() << "face " << i << " differs at glyph " << index;
        }
      }
      mapped.ReleaseFaces();
      if (map.Stretches() != 1) {
        return ::testing::AssertionFailure() << "face " << i << " was not given up";
      }
    }
  }
  if (map.Stretches() != 0) {
    return ::testing::AssertionFailure() << "bytes were left mapped when the image was gone";
  }
  return ::testing::AssertionSuccess();
}

TEST(FontImage, OpenedThroughAMapKeepsOnlyItsHeaderAndItsFacesReadable) {
  const std::vector<std::uint8_t> image =
      FontImageBytes({SmallFace(), SmallFace(0, kItalic, 20), SmallFace(1, kBold, 30)});
  bn::core::MemoryByteMap map(image);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  EXPECT_EQ(fonts.Status(), FontStatus::kOk);
  EXPECT_EQ(map.Stretches(), 1U);
  EXPECT_EQ(map.MappedBytes(), 16U + (3 * 48U));
  EXPECT_EQ(fonts.Held(), 0U);
  EXPECT_EQ(fonts.HeldBytes(), 0U);

  // A face says what it is, and is found by it, with nothing more mapped.
  ASSERT_EQ(fonts.Faces(), 3U);
  const Face sans = fonts.Find(FaceRole::kSans, kBold, 30);
  ASSERT_TRUE(sans.Valid());
  EXPECT_EQ(sans.Size(), 30);
  EXPECT_EQ(sans.Glyphs(), 8U);
  EXPECT_EQ(fonts.Nearest(FaceRole::kSerif, kBold | kItalic, 20).Style(), kItalic);
  EXPECT_FALSE(fonts.Find(FaceRole::kMono, 0, 20).Valid());
  EXPECT_EQ(map.Stretches(), 1U);
  EXPECT_EQ(map.Maps(), 2U);  // The header by itself, and then with the faces.
  // And has no glyphs yet.
  EXPECT_FALSE(sans.Held());
  Glyph glyph;
  EXPECT_FALSE(sans.Find('a', glyph));
  EXPECT_FALSE(sans.GlyphAt(1, glyph));
  EXPECT_FALSE(sans.Missing(glyph));
}

TEST(FontImage, GivesAHeldFacesGlyphsAsTheImageOpenedWholeGivesThem) {
  ImageFace many = SmallFace(1, kItalic, 24);
  for (std::uint32_t i = 0; i < 40; ++i) {
    many.glyphs[0x100 + (i * 3)] = Block(static_cast<std::uint16_t>(30 + i), 5, 6, 2);
  }
  std::vector<ImageFacePlaces> places;
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), many, SmallFace(3)}, &places);
  EXPECT_TRUE(SameThroughAMap(image));
  // An image with no faces, and one whose faces have nothing in them.
  EXPECT_TRUE(SameThroughAMap(FontImageBytes({})));
  ImageFace nothing = bn::test::EmptyFace(2, 0, 16);
  EXPECT_TRUE(SameThroughAMap(FontImageBytes({nothing, nothing})));

  // What is mapped for a face is its own tables and pictures, from the
  // first byte of them to the last.
  bn::core::MemoryByteMap map(image);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  Face face = fonts.FaceAt(1);
  ASSERT_TRUE(fonts.Hold(face));
  const std::size_t stretch = places[1].pictures + places[1].pictureBytes - places[1].ranges;
  EXPECT_EQ(fonts.HeldBytes(), stretch);
  EXPECT_EQ(map.MappedBytes(), 16U + (3 * 48U) + stretch);
  Glyph glyph;
  ASSERT_TRUE(face.Find(0x103, glyph));
  EXPECT_GE(glyph.picture.data(), image.data() + places[1].pictures);
  EXPECT_LE(glyph.picture.data() + glyph.picture.size(), image.data() + places[1].pictures + places[1].pictureBytes);
}

TEST(FontImage, HoldsAFaceOnceHoweverOftenItIsAsked) {
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1)});
  bn::core::MemoryByteMap map(image);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  Face face = fonts.FaceAt(1);
  ASSERT_TRUE(fonts.Hold(face));
  const std::size_t maps = map.Maps();
  Glyph first;
  ASSERT_TRUE(face.Find('c', first));

  // Asked again, and of the face found again: the same stretch.
  EXPECT_TRUE(fonts.Hold(face));
  Face again = fonts.Find(FaceRole::kSans, 0, 20);
  EXPECT_TRUE(again.Held());  // Found while it is held, it is held.
  EXPECT_TRUE(fonts.Hold(again));
  Glyph second;
  ASSERT_TRUE(again.Find('c', second));
  EXPECT_EQ(second.picture.data(), first.picture.data());
  EXPECT_EQ(fonts.Held(), 1U);
  EXPECT_EQ(map.Maps(), maps);
  EXPECT_EQ(map.Stretches(), 2U);
  // The other face is not held for it.
  EXPECT_FALSE(fonts.FaceAt(0).Held());
}

TEST(FontImage, GivesUpTheFacesItHoldsAndEverythingWhenItCloses) {
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1), SmallFace(2)});
  bn::test::CopyingByteMap map(image);
  {
    FontImage fonts;
    ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
    Face serif = fonts.FaceAt(0);
    Face mono = fonts.FaceAt(2);
    ASSERT_TRUE(fonts.Hold(serif));
    ASSERT_TRUE(fonts.Hold(mono));
    EXPECT_EQ(fonts.Held(), 2U);
    EXPECT_EQ(map.Stretches(), 3U);

    fonts.ReleaseFaces();
    EXPECT_EQ(fonts.Held(), 0U);
    EXPECT_EQ(fonts.HeldBytes(), 0U);
    EXPECT_EQ(map.Stretches(), 1U);
    // Found again, a face is as it was at first, and can be held again.
    Face again = fonts.FaceAt(0);
    EXPECT_TRUE(again.Valid());
    EXPECT_FALSE(again.Held());
    Glyph glyph;
    EXPECT_FALSE(again.Find('a', glyph));
    ASSERT_TRUE(fonts.Hold(again));
    EXPECT_TRUE(again.Find('a', glyph));

    // Opened again, it has given up what it had: the table of faces too.
    ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
    EXPECT_EQ(fonts.Held(), 0U);
    EXPECT_EQ(map.Stretches(), 1U);
    Face held = fonts.FaceAt(1);
    ASSERT_TRUE(fonts.Hold(held));

    fonts.Close();
    EXPECT_EQ(fonts.Status(), FontStatus::kNotOpen);
    EXPECT_EQ(fonts.Faces(), 0U);
    EXPECT_FALSE(fonts.FaceAt(0).Valid());
    EXPECT_EQ(map.Stretches(), 0U);

    // And an image that goes while it holds faces leaves nothing mapped.
    ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
    Face last = fonts.FaceAt(2);
    ASSERT_TRUE(fonts.Hold(last));
    EXPECT_EQ(map.Stretches(), 2U);
  }
  EXPECT_EQ(map.Stretches(), 0U);
  EXPECT_EQ(map.MappedBytes(), 0U);
}

TEST(FontImage, HoldsOnlySoManyFacesAndOnlyWhatThereIsRoomFor) {
  std::vector<ImageFace> faces;
  faces.reserve(FontImage::kMostHeld + 1);
  for (std::size_t i = 0; i < FontImage::kMostHeld + 1; ++i) {
    faces.push_back(SmallFace(0, 0, static_cast<std::uint16_t>(10 + i)));
  }
  std::vector<ImageFacePlaces> places;
  const std::vector<std::uint8_t> image = FontImageBytes(faces, &places);
  {
    bn::test::CopyingByteMap map(image);
    FontImage fonts;
    ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
    for (std::size_t i = 0; i < FontImage::kMostHeld; ++i) {
      Face face = fonts.FaceAt(i);
      ASSERT_TRUE(fonts.Hold(face)) << i;
    }
    EXPECT_EQ(fonts.Held(), FontImage::kMostHeld);
    // One more is refused, and nothing was mapped for it; one already held
    // is still to be had.
    Face over = fonts.FaceAt(FontImage::kMostHeld);
    const std::size_t asked = map.asked;
    EXPECT_FALSE(fonts.Hold(over));
    EXPECT_FALSE(over.Held());
    EXPECT_EQ(map.asked, asked);
    EXPECT_EQ(fonts.Held(), FontImage::kMostHeld);
    Face held = fonts.FaceAt(3);
    EXPECT_TRUE(fonts.Hold(held));
    // With the others given up, it is held.
    fonts.ReleaseFaces();
    over = fonts.FaceAt(FontImage::kMostHeld);
    EXPECT_TRUE(fonts.Hold(over));
  }

  // Room for the table of faces and for one face's stretch, and no more.
  const std::size_t head = 16 + (faces.size() * 48);
  const std::size_t stretch = places[0].pictures + places[0].pictureBytes - places[0].ranges;
  bn::test::CopyingByteMap small(image, head + stretch);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(small), FontStatus::kOk);
  Face first = fonts.FaceAt(0);
  Face second = fonts.FaceAt(1);
  ASSERT_TRUE(fonts.Hold(first));
  EXPECT_FALSE(fonts.Hold(second));
  EXPECT_FALSE(second.Held());
  Glyph glyph;
  EXPECT_FALSE(second.Find('a', glyph));
  EXPECT_EQ(fonts.Held(), 1U);
  EXPECT_EQ(small.Stretches(), 2U);
  EXPECT_TRUE(first.Find('a', glyph));  // The one held is as it was.
  fonts.ReleaseFaces();
  second = fonts.FaceAt(1);
  ASSERT_TRUE(fonts.Hold(second));
  EXPECT_TRUE(second.Find('a', glyph));
}

TEST(FontImage, SaysWhenItsTableOfFacesCannotBeHad) {
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1)});
  // Room for the header, and not for the header with the faces.
  bn::test::CopyingByteMap small(image, 16 + 48);
  FontImage fonts;
  EXPECT_EQ(fonts.Open(small), FontStatus::kNoRoom);
  EXPECT_STREQ(bn::draw::FontStatusName(fonts.Status()), "no room");
  EXPECT_EQ(fonts.Faces(), 0U);
  EXPECT_EQ(small.Stretches(), 0U);
  // Nor for the header.
  bn::test::CopyingByteMap none(image, 8);
  EXPECT_EQ(fonts.Open(none), FontStatus::kNoRoom);
  EXPECT_EQ(none.Stretches(), 0U);
  // Bytes too few to be an image are a bad one, whatever room there is.
  const std::vector<std::uint8_t> few(image.begin(), image.begin() + 15);
  bn::test::CopyingByteMap tiny(few);
  EXPECT_EQ(fonts.Open(tiny), FontStatus::kBadImage);
  EXPECT_EQ(tiny.asked, 0U);
}

TEST(FontImage, HoldsNoFaceThatIsNotItsOwn) {
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1)});
  // Each map gives copies of its own, so the two images' tables of faces
  // are in different places, as two images' are.
  bn::test::CopyingByteMap map(image);
  bn::test::CopyingByteMap otherMap(image);
  FontImage fonts;
  FontImage other;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  ASSERT_EQ(other.Open(otherMap), FontStatus::kOk);
  Face none;
  EXPECT_FALSE(fonts.Hold(none));
  Face foreign = other.FaceAt(1);
  EXPECT_FALSE(fonts.Hold(foreign));
  EXPECT_FALSE(foreign.Held());
  EXPECT_EQ(fonts.Held(), 0U);
  EXPECT_EQ(map.Stretches(), 1U);
  // Nor one of an image since closed.
  Face stale = fonts.FaceAt(0);
  fonts.Close();
  EXPECT_FALSE(fonts.Hold(stale));

  // Every face of an image opened as a span is held from the first, and
  // holding one changes nothing.
  FontImage whole;
  ASSERT_EQ(whole.Open(image), FontStatus::kOk);
  Face face = whole.FaceAt(1);
  EXPECT_TRUE(face.Held());
  EXPECT_TRUE(whole.Hold(face));
  EXPECT_EQ(whole.Held(), 0U);
  EXPECT_EQ(whole.HeldBytes(), 0U);
  whole.ReleaseFaces();
  Glyph glyph;
  EXPECT_TRUE(face.Find('a', glyph));
  EXPECT_FALSE(whole.Hold(foreign));
}

TEST(FontImage, GivesTheBytesOfWhatIsReadableAndNoOthers) {
  std::vector<ImageFacePlaces> places;
  const std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1)}, &places);
  bn::test::CopyingByteMap map(image);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  Face sans = fonts.FaceAt(1);
  ASSERT_TRUE(fonts.Hold(sans));
  Glyph glyph;
  ASSERT_TRUE(sans.Find('e', glyph));
  ASSERT_FALSE(glyph.picture.empty());

  // A picture again, from where the glyph said it is.
  const std::span<const std::uint8_t> again = fonts.Bytes(glyph.pictureAt, glyph.picture.size());
  EXPECT_EQ(again.data(), glyph.picture.data());
  EXPECT_EQ(again.size(), glyph.picture.size());
  // All of the held face's stretch, and nothing that runs out of it.
  const std::size_t begins = places[1].ranges;
  const std::size_t ends = places[1].pictures + places[1].pictureBytes;
  EXPECT_EQ(fonts.Bytes(begins, ends - begins).size(), ends - begins);
  EXPECT_EQ(fonts.Bytes(ends - 1, 1).size(), 1U);
  EXPECT_TRUE(fonts.Bytes(begins - 1, 2).empty());
  EXPECT_TRUE(fonts.Bytes(ends - 1, 2).empty());
  EXPECT_TRUE(fonts.Bytes(begins, SIZE_MAX).empty());
  // Nothing of the face that is not held, nor of the table of faces.
  EXPECT_TRUE(fonts.Bytes(places[0].pictures, 1).empty());
  EXPECT_TRUE(fonts.Bytes(0, 16).empty());
  // And nothing once the face is given up.
  fonts.ReleaseFaces();
  EXPECT_TRUE(fonts.Bytes(glyph.pictureAt, 1).empty());

  // Of an image opened as a span: any of it, and nothing past its end.
  FontImage whole;
  ASSERT_EQ(whole.Open(image), FontStatus::kOk);
  EXPECT_EQ(whole.Bytes(0, image.size()).data(), image.data());
  EXPECT_EQ(whole.Bytes(places[0].pictures, 3).data(), image.data() + places[0].pictures);
  EXPECT_TRUE(whole.Bytes(image.size(), 1).empty());
  EXPECT_TRUE(whole.Bytes(1, image.size()).empty());
  EXPECT_TRUE(whole.Bytes(SIZE_MAX, 2).empty());
}

TEST(FontImage, HoldsAFaceWhoseTablesAreApartAsOneStretch) {
  // The first face's pictures are made the third face's, which lie after
  // the second's: so its stretch runs from its own ranges over the whole
  // of the second face to the end of those pictures.
  std::vector<ImageFacePlaces> places;
  std::vector<std::uint8_t> image = FontImageBytes({SmallFace(), SmallFace(1), SmallFace(2)}, &places);
  PutLe32(image, places[0].record + 28, static_cast<std::uint32_t>(places[2].pictures));
  EXPECT_TRUE(SameThroughAMap(image));

  const std::size_t head = 16 + (3 * 48);
  const std::size_t stretch = places[2].pictures + places[2].pictureBytes - places[0].ranges;
  bn::test::CopyingByteMap map(image, head + stretch);
  FontImage fonts;
  ASSERT_EQ(fonts.Open(map), FontStatus::kOk);
  Face face = fonts.FaceAt(0);
  ASSERT_TRUE(fonts.Hold(face));
  EXPECT_EQ(fonts.HeldBytes(), stretch);
  Glyph glyph;
  ASSERT_TRUE(face.Find('b', glyph));
  EXPECT_GE(glyph.pictureAt, places[2].pictures);
  EXPECT_LE(glyph.pictureAt + glyph.picture.size(), places[2].pictures + places[2].pictureBytes);

  // With a byte less of room than that, it is not held at all.
  bn::test::CopyingByteMap small(image, head + stretch - 1);
  FontImage cramped;
  ASSERT_EQ(cramped.Open(small), FontStatus::kOk);
  Face apart = cramped.FaceAt(0);
  EXPECT_FALSE(cramped.Hold(apart));
  // The second face, whose tables are together, is.
  Face second = cramped.FaceAt(1);
  EXPECT_TRUE(cramped.Hold(second));
}

TEST(FontImage, OpenedThroughAMapRefusesWhatItRefusesWhole) {
  ImageFace many = SmallFace(1, kItalic, 24);
  for (std::uint32_t i = 0; i < 40; ++i) {
    many.glyphs[0x100 + (i * 3)] = Block(static_cast<std::uint16_t>(30 + i), 5, 6, 2);
  }
  const std::vector<std::uint8_t> good = FontImageBytes({SmallFace(), many});
  // Cut short at every byte.
  for (std::size_t length = 0; length < good.size(); ++length) {
    const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
    ASSERT_TRUE(SameThroughAMap(cut)) << length;
  }
  // With more after it, as a partition has.
  std::vector<std::uint8_t> longer = good;
  longer.resize(good.size() + 300, 0xFF);
  EXPECT_TRUE(SameThroughAMap(longer));
  // And with every byte changed three ways: refused if it is refused
  // whole, and if not, every face gives what it gives whole, read from
  // nowhere but its own stretch.
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (const std::uint8_t change : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      std::vector<std::uint8_t> image = good;
      image[at] ^= change;
      ASSERT_TRUE(SameThroughAMap(image)) << at << " " << static_cast<int>(change);
    }
  }
}

}  // namespace
