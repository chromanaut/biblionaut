// bn-font: makes the fonts image the firmware draws text from, out of font
// files a preset names, and says what an image holds, by the code the
// firmware reads it with.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "draw/font.h"
#include "font/build.h"
#include "font/preset.h"
#include "font/sizes.h"

namespace {

using bn::font_tool::BuildImage;
using bn::font_tool::Built;
using bn::font_tool::BuiltFace;
using bn::font_tool::Preset;
using bn::font_tool::ReadPreset;
using bn::font_tool::RoleName;
using bn::font_tool::StyleName;

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s build PRESET --out IMAGE\n"
               "       %s info IMAGE [--glyphs U+XXXX[-U+YYYY]...]\n"
               "       %s sizes IMAGE\n"
               "\n"
               "The fonts image: every face the firmware draws text in, at every size.\n"
               "\n"
               "  build  draws the faces a preset names at the sizes it gives, and writes\n"
               "         the image. A preset is a text file:\n"
               "\n"
               "           sizes 26 30 34\n"
               "           serif regular      Serif-Regular.ttf\n"
               "           serif bold-italic  /Library/Fonts/Serif-BoldItalic.ttf\n"
               "           sans italic        Sans-Italic.ttf\n"
               "           mono bold          Mono-Bold.ttf\n"
               "\n"
               "         Roles are serif, sans, mono and fallback; styles regular, italic,\n"
               "         bold and bold-italic. A file not named from / is found from the\n"
               "         preset's folder. `characters U+0020-U+007E ...` chooses what is\n"
               "         drawn; without it, the set the firmware is built to show.\n"
               "\n"
               "  info   says what an image holds: its faces, and with --glyphs what each\n"
               "         has for the characters named: the glyph's number, how far it\n"
               "         moves the pen in quarters of a pixel, its picture's size and\n"
               "         where it lies from the pen, and its levels of ink added up.\n"
               "\n"
               "  sizes  says what an image's pictures would take kept in other ways than\n"
               "         the image keeps them: as runs, compressed a glyph at a time, and\n"
               "         compressed in groups; for each size, and for all of them.\n"
               "\n"
               "Ends with 0 if all went well, 1 if the preset or a font cannot be read, or\n"
               "the image cannot be written or is not one.\n",
               program, program, program);
}

bool ReadFile(const std::string& path, std::string& contents) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "bn-font: cannot read %s\n", path.c_str());
    return false;
  }
  contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

std::string FolderOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

int RunBuild(const std::vector<std::string>& arguments) {
  std::string presetPath;
  std::string outPath;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i] == "--out" && i + 1 < arguments.size()) {
      outPath = arguments[++i];
    } else if (!arguments[i].starts_with("--") && presetPath.empty()) {
      presetPath = arguments[i];
    } else {
      return -1;
    }
  }
  if (presetPath.empty() || outPath.empty()) {
    return -1;
  }
  std::string text;
  if (!ReadFile(presetPath, text)) {
    return 1;
  }
  const Preset preset = ReadPreset(text, FolderOf(presetPath));
  if (!preset.ok) {
    std::fprintf(stderr, "bn-font: %s: %s\n", presetPath.c_str(), preset.error.c_str());
    return 1;
  }
  const Built built = BuildImage(preset);
  for (const std::string& warning : built.warnings) {
    std::fprintf(stderr, "bn-font: warning: %s\n", warning.c_str());
  }
  if (!built.ok) {
    std::fprintf(stderr, "bn-font: %s\n", built.error.c_str());
    return 1;
  }
  std::ofstream out(outPath, std::ios::binary);
  out.write(reinterpret_cast<const char*>(built.image.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(built.image.size()));
  out.close();
  if (!out) {
    std::fprintf(stderr, "bn-font: cannot write %s\n", outPath.c_str());
    return 1;
  }
  std::size_t pictures = 0;
  for (const BuiltFace& face : built.faces) {
    std::printf(
        "%-8s %-11s %2u px: %4zu glyphs (%zu lacking), pictures %7zu bytes; ascent %d, descent %d, line %u, "
        "x-height %u\n",
        std::string(RoleName(face.role)).c_str(), std::string(StyleName(face.style)).c_str(),
        static_cast<unsigned>(face.size), face.glyphs, face.lacking, face.pictureBytes, static_cast<int>(face.ascent),
        static_cast<int>(face.descent), static_cast<unsigned>(face.lineHeight), static_cast<unsigned>(face.xHeight));
    pictures += face.pictureBytes;
  }
  std::printf("faces %zu, pictures %zu bytes, image %zu bytes\n", built.faces.size(), pictures, built.image.size());
  return 0;
}

// Says what `face` has for each of `characters`.
void PrintGlyphs(const bn::draw::Face& face, const std::vector<bn::font_tool::CodePoints>& characters) {
  for (const auto& [first, last] : characters) {
    for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint) {
      bn::draw::Glyph glyph;
      if (!face.Find(codePoint, glyph)) {
        std::printf("  U+%04X: no glyph\n", static_cast<unsigned>(codePoint));
        continue;
      }
      unsigned ink = 0;
      for (std::uint32_t y = 0; y < glyph.height; ++y) {
        for (std::uint32_t x = 0; x < glyph.width; ++x) {
          ink += glyph.Ink(x, y);
        }
      }
      std::printf("  U+%04X: glyph %u, advance %u, %u by %u at %d, %d, ink %u\n", static_cast<unsigned>(codePoint),
                  static_cast<unsigned>(glyph.index), static_cast<unsigned>(glyph.advance),
                  static_cast<unsigned>(glyph.width), static_cast<unsigned>(glyph.height), static_cast<int>(glyph.left),
                  static_cast<int>(glyph.top), ink);
    }
  }
}

int RunInfo(const std::vector<std::string>& arguments) {
  if (arguments.empty() || arguments[0].starts_with("--")) {
    return -1;
  }
  std::vector<bn::font_tool::CodePoints> characters;
  if (arguments.size() > 1) {
    if (arguments[1] != "--glyphs" || arguments.size() == 2) {
      return -1;
    }
    for (std::size_t i = 2; i < arguments.size(); ++i) {
      bn::font_tool::CodePoints codePoints;
      if (!bn::font_tool::ReadCodePoints(arguments[i], codePoints)) {
        return -1;
      }
      characters.push_back(codePoints);
    }
  }
  std::string bytes;
  if (!ReadFile(arguments[0], bytes)) {
    return 1;
  }
  const std::span<const std::uint8_t> image(
      reinterpret_cast<const std::uint8_t*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      bytes.size());
  bn::draw::FontImage fonts;
  const bn::draw::FontStatus status = fonts.Open(image);
  if (status != bn::draw::FontStatus::kOk) {
    std::fprintf(stderr, "bn-font: %s: %s\n", arguments[0].c_str(), bn::draw::FontStatusName(status));
    return 1;
  }
  std::printf("image %zu bytes, faces %zu\n", bytes.size(), fonts.Faces());
  for (std::size_t i = 0; i < fonts.Faces(); ++i) {
    const bn::draw::Face face = fonts.FaceAt(i);
    std::printf("%-8s %-11s %2u px: %4u glyphs; ascent %d, descent %d, line %u, x-height %u\n",
                std::string(RoleName(face.Role())).c_str(), std::string(StyleName(face.Style())).c_str(),
                static_cast<unsigned>(face.Size()), static_cast<unsigned>(face.Glyphs()),
                static_cast<int>(face.Ascent()), static_cast<int>(face.Descent()),
                static_cast<unsigned>(face.LineHeight()), static_cast<unsigned>(face.XHeight()));
    PrintGlyphs(face, characters);
  }
  return 0;
}

// A count of bytes, and what share of `whole` it is.
void PrintShare(const char* name, std::size_t bytes, std::size_t whole) {
  std::printf("  %-22s %9zu bytes  %3zu%%\n", name, bytes, whole == 0 ? 0 : ((bytes * 100) + (whole / 2)) / whole);
}

int RunSizes(const std::vector<std::string>& arguments) {
  if (arguments.size() != 1 || arguments[0].starts_with("--")) {
    return -1;
  }
  std::string bytes;
  if (!ReadFile(arguments[0], bytes)) {
    return 1;
  }
  const bn::font_tool::ImageSizes sizes = bn::font_tool::MeasureImage(
      {reinterpret_cast<const std::uint8_t*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
       bytes.size()});
  if (!sizes.ok) {
    std::fprintf(stderr, "bn-font: %s: %s\n", arguments[0].c_str(), sizes.error.c_str());
    return 1;
  }
  std::printf("image %zu bytes: tables %zu, pictures %zu\n", sizes.imageBytes, sizes.tableBytes,
              sizes.all.pictures.asTheyAre);
  const auto print = [](const bn::font_tool::SizeOfFaces& faces) {
    const bn::font_tool::PictureSizes& pictures = faces.pictures;
    const std::size_t whole = pictures.asTheyAre;
    PrintShare("as they are", pictures.asTheyAre, whole);
    PrintShare("run-length, a glyph", pictures.runLength, whole);
    PrintShare("DEFLATE, a glyph", pictures.deflate, whole);
    PrintShare("the best, a glyph", pictures.best, whole);
    PrintShare("DEFLATE, groups of 16", pictures.groupsOf16, whole);
    PrintShare("DEFLATE, groups of 64", pictures.groupsOf64, whole);
    PrintShare("DEFLATE, groups of 256", pictures.groupsOf256, whole);
    PrintShare("DEFLATE, a face", pictures.wholeFaces, whole);
  };
  for (const bn::font_tool::SizeOfFaces& faces : sizes.sizes) {
    std::printf("%u px: %zu faces, %zu glyphs\n", static_cast<unsigned>(faces.size), faces.faces, faces.glyphs);
    print(faces);
  }
  std::printf("all: %zu faces, %zu glyphs\n", sizes.all.faces, sizes.all.glyphs);
  print(sizes.all);
  std::printf("the best, a glyph: %zu as they are, %zu run-length, %zu DEFLATE\n", sizes.all.pictures.bestAsTheyAre,
              sizes.all.pictures.bestRunLength, sizes.all.pictures.bestDeflate);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> all(argv, argv + argc);
  int result = -1;
  if (all.size() >= 2) {
    const std::vector<std::string> arguments(all.begin() + 2, all.end());
    if (all[1] == "build") {
      result = RunBuild(arguments);
    } else if (all[1] == "info") {
      result = RunInfo(arguments);
    } else if (all[1] == "sizes") {
      result = RunSizes(arguments);
    }
  }
  if (result < 0) {
    PrintUsage(all.empty() ? "bn-font" : all[0].c_str());
    return 1;
  }
  return result;
}
