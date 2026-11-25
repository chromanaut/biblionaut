#include "draw/font.h"

#include "core/endian.h"

namespace bn::draw {

namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFaceBytes = 48;
constexpr std::size_t kRangeBytes = 12;
constexpr std::size_t kGlyphBytes = 12;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kLastRole = 3;
constexpr std::uint8_t kLastStyle = kBold | kItalic;

// Numbers at `at` in `bytes`. Whoever calls has seen that they are there.
std::uint16_t Read16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe16(bytes.subspan(at).first<2>());
}
std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return core::LoadLe32(bytes.subspan(at).first<4>());
}

// Whether `count` things of `each` bytes from `at` lie inside `size` bytes.
bool Inside(std::uint64_t at, std::uint64_t count, std::uint64_t each, std::uint64_t size) noexcept {
  return at <= size && count * each <= size - at;
}

}  // namespace

const char* FontStatusName(FontStatus status) noexcept {
  switch (status) {
    case FontStatus::kOk:
      return "ok";
    case FontStatus::kNotOpen:
      return "not open";
    case FontStatus::kBadImage:
      return "bad image";
  }
  return "unknown";
}

// ---- A face ----

FaceRole Face::Role() const noexcept { return Valid() ? static_cast<FaceRole>(record_[0]) : FaceRole::kSerif; }
std::uint8_t Face::Style() const noexcept { return Valid() ? record_[1] : 0; }
std::uint16_t Face::Size() const noexcept { return Valid() ? Read16(record_, 2) : 0; }
std::int16_t Face::Ascent() const noexcept { return Valid() ? static_cast<std::int16_t>(Read16(record_, 4)) : 0; }
std::int16_t Face::Descent() const noexcept { return Valid() ? static_cast<std::int16_t>(Read16(record_, 6)) : 0; }
std::uint16_t Face::LineHeight() const noexcept { return Valid() ? Read16(record_, 8) : 0; }
std::uint16_t Face::XHeight() const noexcept { return Valid() ? Read16(record_, 10) : 0; }
std::uint32_t Face::Glyphs() const noexcept { return Valid() ? Read32(record_, 20) : 0; }

bool Face::Find(std::uint32_t codePoint, Glyph& glyph) const noexcept {
  steps_ = 0;
  if (!Valid()) {
    return false;
  }
  const std::uint32_t ranges = Read32(record_, 12);
  const std::uint32_t rangesAt = Read32(record_, 16);
  // The ranges are in order: halving, as long as there are any left. Each
  // step halves what is left, so 2^32 ranges take 33 at the most.
  std::uint32_t low = 0;
  std::uint32_t high = ranges;
  while (low < high) {
    ++steps_;
    const std::uint32_t middle = low + ((high - low) / 2);
    const std::size_t at = rangesAt + (std::size_t{middle} * kRangeBytes);
    const std::uint32_t first = Read32(image_, at);
    const std::uint32_t count = Read32(image_, at + 4);
    if (codePoint < first) {
      high = middle;
    } else if (codePoint - first >= count) {
      low = middle + 1;
    } else {
      const std::uint64_t index = std::uint64_t{Read32(image_, at + 8)} + (codePoint - first);
      return index <= UINT32_MAX && GlyphAt(static_cast<std::uint32_t>(index), glyph);
    }
  }
  return false;
}

bool Face::GlyphAt(std::uint32_t index, Glyph& glyph) const noexcept {
  if (!Valid() || index >= Glyphs()) {
    return false;
  }
  const std::size_t at = Read32(record_, 24) + (std::size_t{index} * kGlyphBytes);
  const std::uint8_t width = image_[at + 2];
  const std::uint8_t height = image_[at + 3];
  const std::uint32_t picture = Read32(image_, at + 8);
  const std::size_t bytes = PictureBytes(width, height);
  const std::uint32_t pictures = Read32(record_, 32);
  if (!Inside(picture, bytes, 1, pictures)) {
    return false;
  }
  glyph = Glyph{.index = index,
                .advance = Read16(image_, at),
                .width = width,
                .height = height,
                .left = static_cast<std::int8_t>(image_[at + 4]),
                .top = static_cast<std::int8_t>(image_[at + 5]),
                .picture = image_.subspan(std::size_t{Read32(record_, 28)} + picture, bytes)};
  return true;
}

// ---- The image ----

FontStatus FontImage::Open(std::span<const std::uint8_t> image) noexcept {
  image_ = {};
  faces_ = 0;
  status_ = FontStatus::kBadImage;
  if (image.size() < kHeaderBytes || image[0] != 'B' || image[1] != 'N' || image[2] != 'F' || image[3] != 'N' ||
      image[4] != kVersion) {
    return status_;
  }
  const std::size_t faces = Read16(image, 6);
  const std::uint32_t bytes = Read32(image, 8);
  if (faces > kMostFaces || bytes > image.size() || !Inside(kHeaderBytes, faces, kFaceBytes, bytes)) {
    return status_;
  }
  const std::span<const std::uint8_t> kept = image.first(bytes);
  // Each face's tables have to lie inside the image, so that what is found
  // in them later needs only its own place looked at.
  for (std::size_t i = 0; i < faces; ++i) {
    const std::span<const std::uint8_t> record = kept.subspan(kHeaderBytes + (i * kFaceBytes), kFaceBytes);
    if (record[0] > kLastRole || record[1] > kLastStyle ||
        !Inside(Read32(record, 16), Read32(record, 12), kRangeBytes, bytes) ||
        !Inside(Read32(record, 24), Read32(record, 20), kGlyphBytes, bytes) ||
        !Inside(Read32(record, 28), Read32(record, 32), 1, bytes)) {
      return status_;
    }
  }
  image_ = kept;
  faces_ = faces;
  status_ = FontStatus::kOk;
  return status_;
}

Face FontImage::FaceAt(std::size_t index) const noexcept {
  if (index >= faces_) {
    return {};
  }
  return {image_, image_.subspan(kHeaderBytes + (index * kFaceBytes), kFaceBytes)};
}

Face FontImage::Find(FaceRole role, std::uint8_t style, std::uint16_t size) const noexcept {
  for (std::size_t i = 0; i < faces_; ++i) {
    const Face face = FaceAt(i);
    if (face.Role() == role && face.Style() == style && face.Size() == size) {
      return face;
    }
  }
  return {};
}

Face FontImage::Nearest(FaceRole role, std::uint8_t style, std::uint16_t size) const noexcept {
  // Bold italic, then bold, then italic; any of them, then regular.
  for (const std::uint8_t tried :
       {style, static_cast<std::uint8_t>(style & kBold), static_cast<std::uint8_t>(style & kItalic), std::uint8_t{0}}) {
    const Face face = Find(role, tried, size);
    if (face.Valid()) {
      return face;
    }
  }
  return {};
}

}  // namespace bn::draw
