#include "draw/svg_transform.h"

#include <array>
#include <cstddef>

#include "draw/svg_number.h"

namespace bn::draw {

namespace {

void SkipSpace(std::string_view text, std::size_t& at) noexcept {
  while (at < text.size() && IsSvgSpace(text[at])) {
    ++at;
  }
}

// Space, and one comma if there is one.
void SkipSeparator(std::string_view text, std::size_t& at) noexcept {
  SkipSpace(text, at);
  if (at < text.size() && text[at] == ',') {
    ++at;
    SkipSpace(text, at);
  }
}

// One of the list: its name and the numbers in its brackets. False if what
// is at `at` is not one.
bool ReadOne(std::string_view text, std::size_t& at, Matrix& one, bool& clamped) noexcept {
  const std::size_t nameStart = at;
  while (at < text.size() && ((text[at] >= 'a' && text[at] <= 'z') || (text[at] >= 'A' && text[at] <= 'Z'))) {
    ++at;
  }
  const std::string_view name(text.data() + nameStart, at - nameStart);
  SkipSpace(text, at);
  if (at >= text.size() || text[at] != '(') {
    return false;
  }
  ++at;

  std::array<Number, 6> numbers{};
  std::size_t count = 0;
  while (true) {
    SkipSpace(text, at);
    if (at < text.size() && text[at] == ')') {
      ++at;
      break;
    }
    if (count > 0) {
      SkipSeparator(text, at);
    }
    if (count == numbers.size() || !ScanNumber(text, at, numbers[count], clamped)) {
      return false;
    }
    ++count;
  }

  const auto fixed = [&numbers, &clamped](std::size_t index) noexcept { return FixedOf(numbers[index], clamped); };
  const auto factor = [&numbers, &clamped](std::size_t index) noexcept {
    return CoefficientOf(numbers[index], clamped);
  };
  if (name == "matrix" && count == 6) {
    one.a = factor(0);
    one.b = factor(1);
    one.c = factor(2);
    one.d = factor(3);
    one.e = fixed(4);
    one.f = fixed(5);
    return true;
  }
  if (name == "translate" && (count == 1 || count == 2)) {
    one = Translation(fixed(0), count == 2 ? fixed(1) : 0);
    return true;
  }
  if (name == "scale" && (count == 1 || count == 2)) {
    one = Scaling(factor(0), factor(count == 2 ? 1 : 0));
    return true;
  }
  if (name == "rotate" && (count == 1 || count == 3)) {
    one = Rotation(AngleOfDegrees(numbers[0]));
    if (count == 3) {
      // About a point: the point is brought to (0, 0), turned about, and
      // put back.
      const Fixed x = fixed(1);
      const Fixed y = fixed(2);
      one = Multiply(Translation(x, y), Multiply(one, Translation(-x, -y), clamped), clamped);
    }
    return true;
  }
  if (name == "skewX" && count == 1) {
    one = SkewX(AngleOfDegrees(numbers[0]), clamped);
    return true;
  }
  if (name == "skewY" && count == 1) {
    one = SkewY(AngleOfDegrees(numbers[0]), clamped);
    return true;
  }
  return false;
}

}  // namespace

bool ParseSvgTransform(std::string_view text, Matrix& matrix, bool& clamped) noexcept {
  Matrix all;
  bool held = false;
  std::size_t at = 0;
  SkipSpace(text, at);
  while (at < text.size()) {
    Matrix one;
    if (!ReadOne(text, at, one, held)) {
      return false;
    }
    all = Multiply(all, one, held);
    SkipSeparator(text, at);
  }
  matrix = all;
  clamped = clamped || held;
  return true;
}

}  // namespace bn::draw
