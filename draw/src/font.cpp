#include "draw/font.h"

#include <algorithm>

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

// What an image's first bytes say of it: how many faces it has, and how
// long it is. False if they are not a fonts image's, or the table of faces
// would not be inside it.
bool ReadHeader(std::span<const std::uint8_t> header, std::size_t& faces, std::uint32_t& bytes) noexcept {
  if (header.size() < kHeaderBytes || header[0] != 'B' || header[1] != 'N' || header[2] != 'F' || header[3] != 'N' ||
      header[4] != kVersion) {
    return false;
  }
  faces = Read16(header, 6);
  bytes = Read32(header, 8);
  return faces <= FontImage::kMostFaces && Inside(kHeaderBytes, faces, kFaceBytes, bytes);
}

// Whether each of the `faces` records in `head` is a face's, with its
// tables inside an image of `bytes`: so that what is found in them later
// needs only its own place looked at.
bool RecordsGood(std::span<const std::uint8_t> head, std::size_t faces, std::uint32_t bytes) noexcept {
  for (std::size_t i = 0; i < faces; ++i) {
    const std::span<const std::uint8_t> record = head.subspan(kHeaderBytes + (i * kFaceBytes), kFaceBytes);
    if (record[0] > kLastRole || record[1] > kLastStyle ||
        !Inside(Read32(record, 16), Read32(record, 12), kRangeBytes, bytes) ||
        !Inside(Read32(record, 24), Read32(record, 20), kGlyphBytes, bytes) ||
        !Inside(Read32(record, 28), Read32(record, 32), 1, bytes)) {
      return false;
    }
  }
  return true;
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
    case FontStatus::kNoRoom:
      return "no room";
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
  if (!held_) {
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
    const std::span<const std::uint8_t> range = From(rangesAt + (std::size_t{middle} * kRangeBytes));
    const std::uint32_t first = Read32(range, 0);
    const std::uint32_t count = Read32(range, 4);
    if (codePoint < first) {
      high = middle;
    } else if (codePoint - first >= count) {
      low = middle + 1;
    } else {
      const std::uint64_t index = std::uint64_t{Read32(range, 8)} + (codePoint - first);
      return index <= UINT32_MAX && GlyphAt(static_cast<std::uint32_t>(index), glyph);
    }
  }
  return false;
}

bool Face::GlyphAt(std::uint32_t index, Glyph& glyph) const noexcept {
  if (!held_ || index >= Glyphs()) {
    return false;
  }
  const std::span<const std::uint8_t> record = From(Read32(record_, 24) + (std::size_t{index} * kGlyphBytes));
  const std::uint8_t width = record[2];
  const std::uint8_t height = record[3];
  const std::uint32_t picture = Read32(record, 8);
  const std::size_t bytes = PictureBytes(width, height);
  const std::uint32_t pictures = Read32(record_, 32);
  if (!Inside(picture, bytes, 1, pictures)) {
    return false;
  }
  // Among the face's pictures, which are inside the image: so where it
  // begins is no more than the image is long, which 32 bits hold.
  const std::uint32_t at = Read32(record_, 28) + picture;
  glyph = Glyph{.index = index,
                .advance = Read16(record, 0),
                .width = width,
                .height = height,
                .left = static_cast<std::int8_t>(record[4]),
                .top = static_cast<std::int8_t>(record[5]),
                .picture = From(at).first(bytes),
                .pictureAt = at};
  return true;
}

// ---- The image ----

FontStatus FontImage::Open(std::span<const std::uint8_t> image) noexcept {
  Close();
  status_ = FontStatus::kBadImage;
  std::size_t faces = 0;
  std::uint32_t bytes = 0;
  if (!ReadHeader(image, faces, bytes) || bytes > image.size() || !RecordsGood(image, faces, bytes)) {
    return status_;
  }
  head_ = image.first(bytes);
  faces_ = faces;
  status_ = FontStatus::kOk;
  return status_;
}

FontStatus FontImage::Open(core::ByteMap& map) noexcept {
  Close();
  status_ = FontStatus::kBadImage;
  const std::size_t size = map.Size();
  if (size < kHeaderBytes) {
    return status_;
  }
  // The header by itself first: it says how long the table of faces is.
  const std::span<const std::uint8_t> header = map.Map(0, kHeaderBytes);
  if (header.empty()) {
    status_ = FontStatus::kNoRoom;
    return status_;
  }
  std::size_t faces = 0;
  std::uint32_t bytes = 0;
  const bool good = ReadHeader(header, faces, bytes);
  map.Unmap(header);
  if (!good || bytes > size) {
    return status_;
  }
  const std::span<const std::uint8_t> head = map.Map(0, kHeaderBytes + (faces * kFaceBytes));
  if (head.empty()) {
    status_ = FontStatus::kNoRoom;
    return status_;
  }
  if (!RecordsGood(head, faces, bytes)) {
    map.Unmap(head);
    return status_;
  }
  map_ = &map;
  head_ = head;
  faces_ = faces;
  status_ = FontStatus::kOk;
  return status_;
}

void FontImage::Close() noexcept {
  ReleaseFaces();
  if (map_ != nullptr) {
    map_->Unmap(head_);
  }
  map_ = nullptr;
  head_ = {};
  faces_ = 0;
  status_ = FontStatus::kNotOpen;
}

bool FontImage::Hold(Face& face) noexcept {
  // A face of this image has the record the image has at its number.
  if (!face.Valid() || face.index_ >= faces_ ||
      face.record_.data() != head_.subspan(kHeaderBytes + (face.index_ * kFaceBytes)).data()) {
    return false;
  }
  if (map_ == nullptr) {
    return true;
  }
  for (std::size_t i = 0; i < held_; ++i) {
    if (heldFaces_[i].index == face.index_) {
      face.bytes_ = heldFaces_[i].bytes;
      face.base_ = heldFaces_[i].base;
      face.held_ = true;
      return true;
    }
  }
  if (held_ == kMostHeld) {
    return false;
  }
  // Its stretch of the image: from where the first of its three tables
  // begins to where the last of them ends. Opening the image saw that each
  // is inside it.
  const std::span<const std::uint8_t> record = face.record_;
  const std::uint64_t rangesAt = Read32(record, 16);
  const std::uint64_t glyphsAt = Read32(record, 24);
  const std::uint64_t picturesAt = Read32(record, 28);
  const std::uint64_t begins = std::min({rangesAt, glyphsAt, picturesAt});
  const std::uint64_t ends =
      std::max({rangesAt + (std::uint64_t{Read32(record, 12)} * kRangeBytes),
                glyphsAt + (std::uint64_t{Read32(record, 20)} * kGlyphBytes), picturesAt + Read32(record, 32)});
  std::span<const std::uint8_t> stretch;
  if (ends > begins) {
    stretch = map_->Map(static_cast<std::size_t>(begins), static_cast<std::size_t>(ends - begins));
    if (stretch.empty()) {
      return false;
    }
  }
  heldFaces_[held_] = HeldFace{.index = face.index_, .bytes = stretch, .base = static_cast<std::size_t>(begins)};
  ++held_;
  face.bytes_ = stretch;
  face.base_ = static_cast<std::size_t>(begins);
  face.held_ = true;
  return true;
}

void FontImage::ReleaseFaces() noexcept {
  for (std::size_t i = 0; i < held_; ++i) {
    map_->Unmap(heldFaces_[i].bytes);
    heldFaces_[i] = HeldFace{};
  }
  held_ = 0;
}

std::size_t FontImage::HeldBytes() const noexcept {
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < held_; ++i) {
    bytes += heldFaces_[i].bytes.size();
  }
  return bytes;
}

std::span<const std::uint8_t> FontImage::Bytes(std::size_t at, std::size_t length) const noexcept {
  if (map_ == nullptr) {
    return Inside(at, length, 1, head_.size()) ? head_.subspan(at, length) : std::span<const std::uint8_t>{};
  }
  for (std::size_t i = 0; i < held_; ++i) {
    const HeldFace& held = heldFaces_[i];
    if (at >= held.base && Inside(at - held.base, length, 1, held.bytes.size())) {
      return held.bytes.subspan(at - held.base, length);
    }
  }
  return {};
}

Face FontImage::FaceAt(std::size_t index) const noexcept {
  if (index >= faces_) {
    return {};
  }
  Face face(head_.subspan(kHeaderBytes + (index * kFaceBytes), kFaceBytes), index);
  if (map_ == nullptr) {
    face.bytes_ = head_;
    face.held_ = true;
    return face;
  }
  for (std::size_t i = 0; i < held_; ++i) {
    if (heldFaces_[i].index == index) {
      face.bytes_ = heldFaces_[i].bytes;
      face.base_ = heldFaces_[i].base;
      face.held_ = true;
    }
  }
  return face;
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
