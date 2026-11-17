// The fuzz target for the small things an SVG file's attributes are
// written in (draw/svg_values.h), and for a transform
// (draw/svg_transform.h).
//
// What it is given:
//
//   byte 0        bits 0 to 2: what the text is read as: a length (and a
//                 list of them), a colour, a paint, an opacity, a view box,
//                 how a view box is fitted, a transform, or each of them in
//                 turn
//   bytes 1 to 8  a box and the space it is to be fitted into: for each,
//                 where it begins across and down, and how wide and how
//                 high it is
//   the rest      the text of an attribute, or what passes for one
//
// What has to hold, whatever the bytes are:
//
//   - a reading that refuses the text leaves what it was to fill as it
//     was;
//   - space before and after the text changes nothing;
//   - a list of lengths has no more than it has room for, and some; and
//     what reads as one length reads as a list of that one;
//   - a colour reads the same in capitals;
//   - a paint that is a colour is that colour, and one that says to take
//     what the element is in has nothing else to say;
//   - a percentage of nothing is nothing;
//   - a transform of nothing but space is one, and changes nothing;
//   - a box is fitted where SVG says it goes, worked out here in floating
//     point: within a thirtieth of a pixel at its corners.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "draw/fixed.h"
#include "draw/svg_transform.h"
#include "draw/svg_values.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Matrix;
using bn::draw::SvgBox;
using bn::draw::SvgFit;
using bn::draw::SvgLength;
using bn::draw::SvgPaint;
using bn::draw::SvgUnit;

enum Reading : std::uint8_t { kLength, kColour, kPaint, kOpacity, kViewBox, kFit, kTransform, kAll };

// The text with space of every kind before and after it.
std::string Spaced(std::string_view text) { return " \t" + std::string(text) + "\r\n\f "; }

std::string InCapitals(std::string_view text) {
  std::string capitals(text);
  for (char& c : capitals) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return capitals;
}

void ReadLength(std::string_view text) {
  const SvgLength untouched = {.value = 123, .unit = SvgUnit::kPc};
  SvgLength length = untouched;
  bool clamped = false;
  const bool read = bn::draw::ParseSvgLength(text, length, clamped);
  if (!read) {
    BN_FUZZ_CHECK(length.value == untouched.value && length.unit == untouched.unit,
                  "a length that was refused changed what it was to fill");
  }
  SvgLength spaced = untouched;
  bool spacedClamped = false;
  BN_FUZZ_CHECK(bn::draw::ParseSvgLength(Spaced(text), spaced, spacedClamped) == read && spaced.value == length.value &&
                    spaced.unit == length.unit && spacedClamped == clamped,
                "space round a length changed how it was read");
  if (read) {
    BN_FUZZ_CHECK(length.unit <= SvgUnit::kEx, "a length has a unit there is not");
    // As pixels, of a width of nothing and of a page's: whatever the
    // number, it comes to something a position can hold.
    bool held = false;
    const Fixed ofNothing = bn::draw::LengthIn(length, 0, held);
    (void)bn::draw::LengthIn(length, 480 * kFixedOne, held);
    BN_FUZZ_CHECK(length.unit != SvgUnit::kPercent || ofNothing == 0, "a percentage of nothing is something");
  }
}

void ReadLengths(std::string_view text) {
  std::array<SvgLength, 8> lengths{};
  std::size_t count = 0;
  bool clamped = false;
  const bool read = bn::draw::ParseSvgLengths(text, lengths, count, clamped);
  BN_FUZZ_CHECK(!read || (count > 0 && count <= lengths.size()),
                "a list has more lengths than it has room for, or none");
  std::array<SvgLength, 8> spaced{};
  std::size_t spacedCount = 0;
  bool spacedClamped = false;
  BN_FUZZ_CHECK(bn::draw::ParseSvgLengths(Spaced(text), spaced, spacedCount, spacedClamped) == read,
                "space round a list of lengths changed whether it was read");
  if (read) {
    BN_FUZZ_CHECK(spacedCount == count && spacedClamped == clamped, "space round a list of lengths changed it");
    for (std::size_t i = 0; i < count; ++i) {
      BN_FUZZ_CHECK(
          spaced[i].value == lengths[i].value && spaced[i].unit == lengths[i].unit && lengths[i].unit <= SvgUnit::kEx,
          "space round a list of lengths changed one");
    }
  }
  SvgLength one;
  bool oneClamped = false;
  if (bn::draw::ParseSvgLength(text, one, oneClamped)) {
    BN_FUZZ_CHECK(read && count == 1 && lengths[0].value == one.value && lengths[0].unit == one.unit,
                  "what reads as a length does not read as a list of one");
  }
}

void ReadColour(std::string_view text) {
  std::uint8_t gray = 77;
  std::uint8_t alpha = 88;
  const bool read = bn::draw::ParseSvgColour(text, gray, alpha);
  if (!read) {
    BN_FUZZ_CHECK(gray == 77 && alpha == 88, "a colour that was refused changed what it was to fill");
  }
  for (const std::string& other : {Spaced(text), InCapitals(text)}) {
    std::uint8_t otherGray = 77;
    std::uint8_t otherAlpha = 88;
    BN_FUZZ_CHECK(
        bn::draw::ParseSvgColour(other, otherGray, otherAlpha) == read && otherGray == gray && otherAlpha == alpha,
        "space round a colour, or capitals, changed how it was read");
  }
}

void ReadPaint(std::string_view text) {
  const SvgPaint untouched = {.kind = SvgPaint::Kind::kCurrent, .gray = 7, .alpha = 8, .server = true};
  SvgPaint paint = untouched;
  const bool read = bn::draw::ParseSvgPaint(text, paint);
  if (!read) {
    BN_FUZZ_CHECK(paint == untouched, "a paint that was refused changed what it was to fill");
  }
  SvgPaint spaced = untouched;
  BN_FUZZ_CHECK(bn::draw::ParseSvgPaint(Spaced(text), spaced) == read && spaced == paint,
                "space round a paint changed how it was read");
  std::uint8_t gray = 0;
  std::uint8_t alpha = 0;
  if (bn::draw::ParseSvgColour(text, gray, alpha)) {
    BN_FUZZ_CHECK(
        read && paint.kind == SvgPaint::Kind::kGray && paint.gray == gray && paint.alpha == alpha && !paint.server,
        "a paint that is a colour was not read as that colour");
  }
  if (read) {
    BN_FUZZ_CHECK(paint.kind <= SvgPaint::Kind::kInherit, "a paint is of a kind there is not");
    BN_FUZZ_CHECK(paint.kind != SvgPaint::Kind::kInherit || !paint.server,
                  "a paint both refers to something and says to take what the element is in");
  }
}

void ReadOpacity(std::string_view text) {
  std::uint8_t opacity = 99;
  const bool read = bn::draw::ParseSvgOpacity(text, opacity);
  if (!read) {
    BN_FUZZ_CHECK(opacity == 99, "an opacity that was refused changed what it was to fill");
  }
  std::uint8_t spaced = 99;
  BN_FUZZ_CHECK(bn::draw::ParseSvgOpacity(Spaced(text), spaced) == read && spaced == opacity,
                "space round an opacity changed how it was read");
}

void ReadViewBox(std::string_view text) {
  const SvgBox untouched = {.x = 1, .y = 2, .width = 3, .height = 4};
  SvgBox box = untouched;
  bool clamped = false;
  const bool read = bn::draw::ParseSvgViewBox(text, box, clamped);
  if (!read) {
    BN_FUZZ_CHECK(box == untouched, "a view box that was refused changed what it was to fill");
  }
  SvgBox spaced = untouched;
  bool spacedClamped = false;
  BN_FUZZ_CHECK(bn::draw::ParseSvgViewBox(Spaced(text), spaced, spacedClamped) == read && spaced == box,
                "space round a view box changed how it was read");
}

// Where a length across or down comes to, by what SVG says of fitting a
// box of `box` into a space of `space` that begins at `spaceFrom`, the
// box beginning at `boxFrom` and being made `scale` times its size.
double Placed(double at, double boxFrom, double box, double spaceFrom, double space, double scale,
              SvgFit::Align align) {
  const double spare = space - (box * scale);
  const double part = align == SvgFit::Align::kMin ? 0.0 : (align == SvgFit::Align::kMid ? 0.5 : 1.0);
  return spaceFrom + (part * spare) + ((at - boxFrom) * scale);
}

void ReadFit(std::string_view text, std::span<const std::uint8_t> numbers) {
  const SvgFit untouched = {.x = SvgFit::Align::kMax, .y = SvgFit::Align::kMin, .stretch = true, .slice = true};
  SvgFit fit = untouched;
  const bool read = bn::draw::ParseSvgFit(text, fit);
  if (!read) {
    BN_FUZZ_CHECK(fit == untouched, "a fitting that was refused changed what it was to fill");
  }
  SvgFit spaced = untouched;
  BN_FUZZ_CHECK(bn::draw::ParseSvgFit(Spaced(text), spaced) == read && spaced == fit,
                "space round a fitting changed how it was read");
  if (!read || numbers.size() < 8) {
    return;
  }

  // A box of up to 9,600 units a side, beginning up to 4,800 either side
  // of nothing, into a space of up to 768 pixels a side.
  constexpr Fixed kBoxStep = 153600;   // 37.5 units.
  constexpr Fixed kSpaceStep = 12288;  // 3 pixels.
  const auto signedByte = [&numbers](std::size_t index) {
    return static_cast<Fixed>(static_cast<std::int8_t>(numbers[index]));
  };
  const auto size = [&numbers](std::size_t index) { return static_cast<Fixed>(numbers[index]) + 1; };
  const SvgBox box = {.x = signedByte(0) * kBoxStep,
                      .y = signedByte(1) * kBoxStep,
                      .width = size(2) * kBoxStep,
                      .height = size(3) * kBoxStep};
  const SvgBox space = {.x = signedByte(4) * kSpaceStep,
                        .y = signedByte(5) * kSpaceStep,
                        .width = size(6) * kSpaceStep,
                        .height = size(7) * kSpaceStep};
  bool clamped = false;
  const Matrix matrix = bn::draw::FitBox(box, space, fit, clamped);
  BN_FUZZ_CHECK(matrix.b == 0 && matrix.c == 0 && matrix.a > 0 && matrix.d > 0,
                "fitting a box turned it, or turned it over");
  BN_FUZZ_CHECK(!clamped, "fitting a box of moderate size ran out of room for a number");

  const auto real = [](Fixed value) { return static_cast<double>(value) / kFixedOne; };
  double scaleX = real(space.width) / real(box.width);
  double scaleY = real(space.height) / real(box.height);
  if (!fit.stretch) {
    scaleX = fit.slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    scaleY = scaleX;
  }
  const SvgFit::Align across = fit.stretch ? SvgFit::Align::kMin : fit.x;
  const SvgFit::Align down = fit.stretch ? SvgFit::Align::kMin : fit.y;
  // Each corner of the box, where the matrix puts it and where it should
  // be.
  for (const bool far : {false, true}) {
    const bn::draw::Point corner = {.x = far ? box.x + box.width : box.x, .y = far ? box.y + box.height : box.y};
    const bn::draw::Point placed = bn::draw::Apply(matrix, corner, clamped);
    const double x =
        Placed(real(corner.x), real(box.x), real(box.width), real(space.x), real(space.width), scaleX, across);
    const double y =
        Placed(real(corner.y), real(box.y), real(box.height), real(space.y), real(space.height), scaleY, down);
    BN_FUZZ_CHECK(std::abs(real(placed.x) - x) <= 1.0 / 30 && std::abs(real(placed.y) - y) <= 1.0 / 30,
                  "a box was not fitted where it should be");
  }
}

void ReadTransform(std::string_view text) {
  Matrix untouched;
  untouched.a = 5;
  untouched.e = 7;
  Matrix matrix = untouched;
  bool clamped = false;
  const bool read = bn::draw::ParseSvgTransform(text, matrix, clamped);
  if (!read) {
    BN_FUZZ_CHECK(matrix == untouched, "a transform that was refused changed what it was to fill");
  }
  Matrix spaced = untouched;
  bool spacedClamped = false;
  BN_FUZZ_CHECK(bn::draw::ParseSvgTransform(Spaced(text), spaced, spacedClamped) == read && spaced == matrix &&
                    spacedClamped == clamped,
                "space round a transform changed how it was read");
  if (bn::draw::WithoutSpace(text).empty()) {
    BN_FUZZ_CHECK(read && matrix == Matrix() && !clamped, "a transform of nothing does something");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned reading = input.Byte() & 7U;
  const std::span<const std::uint8_t> numbers = input.Take(8);
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  const std::string_view text(reinterpret_cast<const char*>(rest.data()), rest.size());

  if (reading == kLength || reading == kAll) {
    ReadLength(text);
    ReadLengths(text);
  }
  if (reading == kColour || reading == kAll) {
    ReadColour(text);
  }
  if (reading == kPaint || reading == kAll) {
    ReadPaint(text);
  }
  if (reading == kOpacity || reading == kAll) {
    ReadOpacity(text);
  }
  if (reading == kViewBox || reading == kAll) {
    ReadViewBox(text);
  }
  if (reading == kFit || reading == kAll) {
    ReadFit(text, numbers);
  }
  if (reading == kTransform || reading == kAll) {
    ReadTransform(text);
  }
  return 0;
}
