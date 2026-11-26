#include "font/build.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "font/kerning.h"

namespace bn::font_tool {

namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFaceBytes = 48;
constexpr std::size_t kRangeBytes = 12;
constexpr std::size_t kGlyphBytes = 12;
constexpr std::uint8_t kVersion = 1;

void Put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<std::uint8_t>(value);
  out[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}
void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  Put16(out, at, value & 0xFFFFU);
  Put16(out, at + 2, value >> 16U);
}
// Fills `out` out to a multiple of four bytes, and says where it then ends.
std::size_t Aligned(std::vector<std::uint8_t>& out) {
  out.resize((out.size() + 3) & ~std::size_t{3}, 0);
  return out.size();
}

// FreeType's library and a font of it, let go of when they go.
class Library {
 public:
  Library() : ok_(FT_Init_FreeType(&library_) == 0) {}
  ~Library() {
    if (ok_) {
      FT_Done_FreeType(library_);
    }
  }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  bool Ok() const { return ok_; }
  FT_Library Get() const { return library_; }

 private:
  FT_Library library_ = nullptr;
  bool ok_ = false;
};

class Font {
 public:
  Font(FT_Library library, const std::string& path) : ok_(FT_New_Face(library, path.c_str(), 0, &face_) == 0) {}
  ~Font() {
    if (ok_) {
      FT_Done_Face(face_);
    }
  }
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  bool Ok() const { return ok_; }
  FT_Face Get() const { return face_; }

 private:
  FT_Face face_ = nullptr;
  bool ok_ = false;
};

// One glyph, drawn.
struct Picture {
  std::uint16_t advance = 0;
  std::uint8_t width = 0;
  std::uint8_t height = 0;
  std::int8_t left = 0;
  std::int8_t top = 0;
  std::vector<std::uint8_t> bytes;
};

// One face, drawn: its glyphs in order, and the code points they are for,
// glyph 0 being for none.
struct DrawnFace {
  BuiltFace built;
  std::vector<Picture> glyphs;
  std::vector<std::uint32_t> codePoints;  // For glyphs 1 on.
  std::vector<std::uint32_t> fontGlyphs;  // The font's own number for each glyph, 0 on.
  KerningClasses kerning;
};

// A table of a font file, by its four letters: empty if the font has none.
std::vector<std::uint8_t> FontTable(FT_Face face, char a, char b, char c, char d) {
  const FT_ULong tag = FT_MAKE_TAG(a, b, c, d);
  FT_ULong length = 0;
  if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) != 0 || length == 0) {
    return {};
  }
  std::vector<std::uint8_t> bytes(length);
  if (FT_Load_Sfnt_Table(face, tag, 0, bytes.data(), &length) != 0) {
    return {};
  }
  return bytes;
}

// The kerning of the font `face`, for the glyphs it has for `characters`.
FontKerning KerningOf(FT_Face face, const std::vector<CodePoints>& characters) {
  std::vector<std::uint16_t> glyphs;
  for (const auto& [first, last] : characters) {
    for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint) {
      const FT_UInt index = FT_Get_Char_Index(face, codePoint);
      if (index != 0 && index <= 0xFFFFU) {
        glyphs.push_back(static_cast<std::uint16_t>(index));
      }
    }
  }
  std::sort(glyphs.begin(), glyphs.end());
  glyphs.erase(std::unique(glyphs.begin(), glyphs.end()), glyphs.end());
  return ReadKerning(FontTable(face, 'G', 'P', 'O', 'S'), FontTable(face, 'G', 'D', 'E', 'F'), glyphs);
}

// Puts `kerning`, which is in the font's units and between the font's
// glyphs, into `drawn` as the classes and the table of a face at `size`.
void KernFace(const FontKerning& kerning, std::uint16_t size, std::uint32_t unitsToTheEm, DrawnFace& drawn) {
  const std::vector<FacePair> pairs = FacePairsOf(kerning, drawn.fontGlyphs, size, unitsToTheEm);
  drawn.kerning = MakeKerningClasses(drawn.glyphs.size(), pairs);
  drawn.built.kernedPairs = drawn.kerning.pairs;
  drawn.built.kernFirsts = drawn.kerning.firsts;
  drawn.built.kernSeconds = drawn.kerning.seconds;
  drawn.built.kerned = drawn.kerning.fits && drawn.kerning.pairs > 0;
}

// Draws glyph `index` of `face`, which has its size set. False, with
// `error` said, if FreeType cannot, or what it draws does not fit a record.
bool Draw(FT_Face face, FT_UInt index, Picture& picture, std::string& error) {
  if (FT_Load_Glyph(face, index, FT_LOAD_TARGET_LIGHT) != 0 ||
      FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT) != 0) {
    error = "FreeType could not draw glyph " + std::to_string(index);
    return false;
  }
  const FT_GlyphSlotRec* slot = face->glyph;
  const FT_Bitmap& bitmap = slot->bitmap;
  if (bitmap.width > 0 && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
    error = "glyph " + std::to_string(index) + " is not drawn in grays (a font of pictures, not outlines?)";
    return false;
  }
  // What a record's signed byte holds.
  constexpr int kLeast = -128;
  constexpr int kMost = 127;
  if (bitmap.width > 255 || bitmap.rows > 255 || slot->bitmap_left < kLeast || slot->bitmap_left > kMost ||
      slot->bitmap_top < kLeast || slot->bitmap_top > kMost) {
    error = "glyph " + std::to_string(index) + " is too large for a record at this size";
    return false;
  }
  // The outline's advance, in 16.16 pixels, made quarters of a pixel.
  const double quarters = std::round(static_cast<double>(slot->linearHoriAdvance) * 4.0 / 65536.0);
  picture.advance = static_cast<std::uint16_t>(std::clamp(quarters, 0.0, 65535.0));
  picture.width = static_cast<std::uint8_t>(bitmap.width);
  picture.height = static_cast<std::uint8_t>(bitmap.rows);
  picture.left = static_cast<std::int8_t>(slot->bitmap_left);
  picture.top = static_cast<std::int8_t>(slot->bitmap_top);
  picture.bytes.assign(draw::PictureBytes(picture.width, picture.height), 0);
  for (unsigned y = 0; y < bitmap.rows; ++y) {
    for (unsigned x = 0; x < bitmap.width; ++x) {
      const unsigned gray = bitmap.buffer[(static_cast<std::ptrdiff_t>(y) * bitmap.pitch) + x];
      const unsigned ink = ((gray * 3) + 127) / 255;
      const std::size_t bit = ((static_cast<std::size_t>(y) * bitmap.width) + x) * 2;
      picture.bytes[bit / 8] = static_cast<std::uint8_t>(picture.bytes[bit / 8] | (ink << (6 - (bit % 8))));
    }
  }
  return true;
}

// Draws one face of the preset at one size.
bool DrawFace(FT_Face face, const FaceFile& file, std::uint16_t size, const std::vector<CodePoints>& characters,
              DrawnFace& drawn, std::string& error) {
  if (FT_Set_Pixel_Sizes(face, 0, size) != 0) {
    error = file.path + ": FreeType could not set the size " + std::to_string(size);
    return false;
  }
  const FT_Size_Metrics& metrics = face->size->metrics;
  drawn.built = BuiltFace{.role = file.role,
                          .style = file.style,
                          .size = size,
                          .ascent = static_cast<std::int16_t>((metrics.ascender + 63) / 64),
                          .descent = static_cast<std::int16_t>((-metrics.descender + 63) / 64),
                          .lineHeight = static_cast<std::uint16_t>((metrics.height + 32) / 64)};
  Picture missing;
  if (!Draw(face, 0, missing, error)) {
    error.insert(0, file.path + ": ");
    return false;
  }
  drawn.built.pictureBytes = missing.bytes.size();
  drawn.glyphs.push_back(std::move(missing));
  drawn.fontGlyphs.push_back(0);
  for (const auto& [first, last] : characters) {
    for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint) {
      const FT_UInt index = FT_Get_Char_Index(face, codePoint);
      if (index == 0) {
        ++drawn.built.lacking;
        continue;
      }
      Picture picture;
      if (!Draw(face, index, picture, error)) {
        error.insert(0, file.path + ": ");
        return false;
      }
      if (codePoint == 'x') {
        drawn.built.xHeight = static_cast<std::uint16_t>(std::max(0, static_cast<int>(picture.top)));
      }
      drawn.built.pictureBytes += picture.bytes.size();
      drawn.glyphs.push_back(std::move(picture));
      drawn.codePoints.push_back(codePoint);
      drawn.fontGlyphs.push_back(index);
    }
  }
  drawn.built.glyphs = drawn.glyphs.size();
  return true;
}

// The code points of `characters` in order, each once.
std::vector<CodePoints> InOrder(std::vector<CodePoints> characters) {
  std::sort(characters.begin(), characters.end());
  std::vector<CodePoints> merged;
  for (const CodePoints& range : characters) {
    if (!merged.empty() && range.first <= merged.back().second + 1) {
      merged.back().second = std::max(merged.back().second, range.second);
    } else {
      merged.push_back(range);
    }
  }
  return merged;
}

// Lays the faces out as an image.
bool Write(const std::vector<DrawnFace>& faces, std::vector<std::uint8_t>& image, std::string& error) {
  if (faces.size() > draw::FontImage::kMostFaces) {
    error = "there are " + std::to_string(faces.size()) + " faces, and an image holds " +
            std::to_string(draw::FontImage::kMostFaces);
    return false;
  }
  image.assign(kHeaderBytes + (faces.size() * kFaceBytes), 0);
  image[0] = 'B';
  image[1] = 'N';
  image[2] = 'F';
  image[3] = 'N';
  image[4] = kVersion;
  Put16(image, 6, static_cast<std::uint32_t>(faces.size()));
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const DrawnFace& face = faces[i];
    // The ranges: code points that follow one another, with glyphs that do.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // First code point, count.
    for (const std::uint32_t codePoint : face.codePoints) {
      if (!ranges.empty() && ranges.back().first + ranges.back().second == codePoint) {
        ++ranges.back().second;
      } else {
        ranges.emplace_back(codePoint, 1);
      }
    }
    const std::size_t rangesAt = Aligned(image);
    std::uint32_t glyph = 1;
    for (const auto& [first, count] : ranges) {
      const std::size_t at = image.size();
      image.resize(at + kRangeBytes, 0);
      Put32(image, at, first);
      Put32(image, at + 4, count);
      Put32(image, at + 8, glyph);
      glyph += count;
    }
    const std::size_t glyphsAt = Aligned(image);
    image.resize(glyphsAt + (face.glyphs.size() * kGlyphBytes), 0);
    const bool kerned = face.built.kerned;
    std::size_t picture = 0;
    for (std::size_t g = 0; g < face.glyphs.size(); ++g) {
      const Picture& drawn = face.glyphs[g];
      const std::size_t at = glyphsAt + (g * kGlyphBytes);
      Put16(image, at, drawn.advance);
      image[at + 2] = drawn.width;
      image[at + 3] = drawn.height;
      image[at + 4] = static_cast<std::uint8_t>(drawn.left);
      image[at + 5] = static_cast<std::uint8_t>(drawn.top);
      image[at + 6] = kerned ? face.kerning.firstOf[g] : 0;
      image[at + 7] = kerned ? face.kerning.secondOf[g] : 0;
      Put32(image, at + 8, static_cast<std::uint32_t>(picture));
      picture += drawn.bytes.size();
    }
    // The kerning lies between the glyphs' records and their pictures, so
    // that the face's stretch of the image is no longer for it.
    const std::size_t kerningAt = image.size();
    if (kerned) {
      for (const std::int8_t quarters : face.kerning.table) {
        image.push_back(static_cast<std::uint8_t>(quarters));
      }
    }
    const std::size_t picturesAt = image.size();
    for (const Picture& drawn : face.glyphs) {
      image.insert(image.end(), drawn.bytes.begin(), drawn.bytes.end());
    }
    const std::size_t record = kHeaderBytes + (i * kFaceBytes);
    image[record] = static_cast<std::uint8_t>(face.built.role);
    image[record + 1] = face.built.style;
    Put16(image, record + 2, face.built.size);
    Put16(image, record + 4, static_cast<std::uint16_t>(face.built.ascent));
    Put16(image, record + 6, static_cast<std::uint16_t>(face.built.descent));
    Put16(image, record + 8, face.built.lineHeight);
    Put16(image, record + 10, face.built.xHeight);
    Put32(image, record + 12, static_cast<std::uint32_t>(ranges.size()));
    Put32(image, record + 16, static_cast<std::uint32_t>(rangesAt));
    Put32(image, record + 20, static_cast<std::uint32_t>(face.glyphs.size()));
    Put32(image, record + 24, static_cast<std::uint32_t>(glyphsAt));
    Put32(image, record + 28, static_cast<std::uint32_t>(picturesAt));
    Put32(image, record + 32, static_cast<std::uint32_t>(picture));
    if (kerned) {
      Put32(image, record + 36, static_cast<std::uint32_t>(kerningAt));
      image[record + 40] = static_cast<std::uint8_t>(face.kerning.firsts);
      image[record + 41] = static_cast<std::uint8_t>(face.kerning.seconds);
    }
  }
  if (image.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "the image would be larger than 4 GB";
    return false;
  }
  Put32(image, 8, static_cast<std::uint32_t>(image.size()));
  return true;
}

}  // namespace

Built BuildImage(const Preset& preset) {
  Built built;
  const Library library;
  if (!library.Ok()) {
    built.error = "FreeType could not be started";
    return built;
  }
  const std::vector<CodePoints> characters =
      InOrder(preset.characters.empty() ? DefaultCharacters() : preset.characters);

  // A style a role is not given is drawn in the nearest the role has. Only
  // the serif and the sans are expected to have all four: a monospace face
  // has a regular and a bold, and the fallback a regular, so their others
  // are not looked for.
  for (const FaceFile& named : preset.faces) {
    if (named.style != 0 || (named.role != draw::FaceRole::kSerif && named.role != draw::FaceRole::kSans)) {
      continue;
    }
    for (std::uint8_t style = 1; style <= (draw::kBold | draw::kItalic); ++style) {
      const bool given = std::any_of(preset.faces.begin(), preset.faces.end(), [&](const FaceFile& face) {
        return face.role == named.role && face.style == style;
      });
      if (!given) {
        built.warnings.push_back(std::string(RoleName(named.role)) + " " + std::string(StyleName(style)) +
                                 " is not named: the firmware draws the nearest style the role has in its place");
      }
    }
  }
  if (std::none_of(preset.faces.begin(), preset.faces.end(), [](const FaceFile& face) { return face.style == 0; })) {
    built.warnings.push_back("no role has a regular face");
  }

  std::vector<FaceFile> files = preset.faces;
  std::sort(files.begin(), files.end(), [](const FaceFile& a, const FaceFile& b) {
    return std::pair(a.role, a.style) < std::pair(b.role, b.style);
  });
  std::vector<DrawnFace> faces;
  for (const FaceFile& file : files) {
    const Font font(library.Get(), file.path);
    if (!font.Ok()) {
      built.error = file.path + ": FreeType cannot read it as a font (named on line " + std::to_string(file.line) + ")";
      return built;
    }
    // The font's kerning is read once, in its own units, and made each
    // size's.
    const FontKerning kerning = KerningOf(font.Get(), characters);
    if (kerning.damaged) {
      built.warnings.push_back(file.path + ": its tables of kerning are cut short, or point outside themselves: " +
                               "what could be read of them is what is used");
    }
    for (const std::uint16_t size : preset.sizes) {
      DrawnFace drawn;
      if (!DrawFace(font.Get(), file, size, characters, drawn, built.error)) {
        return built;
      }
      KernFace(kerning, size, font.Get()->units_per_EM, drawn);
      drawn.built.kernLookupsLeftOut = kerning.lookupsLeftOut;
      drawn.built.kernAdjustmentsLeftOut = kerning.otherAdjustments;
      const std::string which = std::string(RoleName(file.role)) + " " + std::string(StyleName(file.style)) + " at " +
                                std::to_string(size) + " px";
      if (!drawn.kerning.fits) {
        built.warnings.push_back(which + ": its glyphs kern in " + std::to_string(drawn.kerning.firsts) +
                                 " ways as the first of two and " + std::to_string(drawn.kerning.seconds) +
                                 " as the second, and an image holds 255 of each: it is made without kerning");
      }
      if (drawn.kerning.clipped > 0) {
        built.warnings.push_back(which + ": " + std::to_string(drawn.kerning.clipped) +
                                 " pairs are kerned by more than an image holds, 32 pixels, and are held at that");
      }
      built.faces.push_back(drawn.built);
      faces.push_back(std::move(drawn));
    }
  }
  if (!Write(faces, built.image, built.error)) {
    return built;
  }
  built.ok = true;
  return built;
}

}  // namespace bn::font_tool
