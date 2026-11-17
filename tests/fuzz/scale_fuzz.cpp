// The fuzz target for the scaler (draw/scale.h).
//
// What it is given:
//
//   bytes 0 and 1   the old width, and 2 and 3 the old height
//   bytes 4 and 5   the new width, and 6 and 7 the new height
//                   each a number of 16 bits, the low byte first: 0 and 1 as
//                   they are, 65534 as kMaxSide and 65535 as one more, and
//                   anything else brought into 1 to 300
//   byte 8          bit 0: an old row is given while new rows still wait
//                   to be given out, once, at a row the next byte chooses
//                   bit 1: the picture is all one gray, the first pixel's
//   byte 9          where that row is
//   the rest        the old picture's pixels, over and over as far as it
//                   needs them
//
// The sizes are what a picture's header says and what layout asks for, and
// the pixels what the picture holds. What has to hold, whatever they are:
//
//   - a side of nothing or past kMaxSide is refused, and nothing is given;
//   - Row() gives no new row out, and Step() one at the most; the new rows
//     come in order, each as wide as the new picture, as many as it is
//     high, and the scaler is finished once they have;
//   - a row given out of turn stops the scaling and nothing more is given;
//   - every new pixel is within a gray of the same working-out in real
//     numbers, and a picture of one gray stays that gray.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "draw/row_sink.h"
#include "draw/scale.h"
#include "fuzz_support.h"

namespace {

using bn::draw::Scaler;
using bn::draw::ScaleStatus;

// The new pixels the target works out a second way, at the most: past this
// it only checks the counts and the order.
constexpr std::size_t kMostCompared = 200000;

std::uint32_t Side(std::uint32_t number) {
  if (number <= 1) {
    return number;
  }
  if (number >= 65534) {
    return Scaler::kMaxSide + (number - 65534);
  }
  return 1 + (number % 300);
}

class Rows final : public bn::draw::RowSink {
 public:
  void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override {
    BN_FUZZ_CHECK(inStep, "a new row was given out other than by Step()");
    BN_FUZZ_CHECK(y == count, "a new row was given out of its order");
    BN_FUZZ_CHECK(gray.size() == width, "a new row is not as wide as the new picture");
    if (keep) {
      pixels.insert(pixels.end(), gray.begin(), gray.end());
    }
    ++count;
  }

  bool inStep = false;
  bool keep = false;
  std::uint32_t width = 0;
  std::uint32_t count = 0;
  std::vector<std::uint8_t> pixels;
};

// One direction of the working-out in real numbers.
std::vector<double> Line(const std::vector<double>& old, std::uint32_t size) {
  const auto count = static_cast<std::uint32_t>(old.size());
  std::vector<double> out(size);
  for (std::uint32_t at = 0; at < size; ++at) {
    if (size == count) {
      out[at] = old[at];
    } else if (size < count) {
      const double start = static_cast<double>(at) * count / size;
      const double end = static_cast<double>(at + 1) * count / size;
      double sum = 0;
      for (auto i = static_cast<std::uint32_t>(std::floor(start)); i < count && i < end; ++i) {
        sum += old[i] * (std::min<double>(i + 1, end) - std::max<double>(i, start));
      }
      out[at] = sum / (end - start);
    } else {
      const double middle = ((at + 0.5) * count / size) - 0.5;
      if (middle <= 0) {
        out[at] = old[0];
      } else if (middle >= count - 1) {
        out[at] = old[count - 1];
      } else {
        const auto before = static_cast<std::uint32_t>(std::floor(middle));
        out[at] = (old[before] * (before + 1 - middle)) + (old[before + 1] * (middle - before));
      }
    }
  }
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const std::uint32_t oldWidth = Side(input.Number16());
  const std::uint32_t oldHeight = Side(input.Number16());
  const std::uint32_t newWidth = Side(input.Number16());
  const std::uint32_t newHeight = Side(input.Number16());
  const unsigned choices = input.Byte();
  const unsigned outOfTurnAt = input.Byte();
  const std::span<const std::uint8_t> given = input.Rest();
  const bool outOfTurn = (choices & 1U) != 0;
  const bool oneGray = (choices & 2U) != 0;

  // The old picture's pixels, as far as they go; the large ones are made of
  // fewer rows that are walked over again.
  const auto pixel = [&](std::size_t at) -> std::uint8_t {
    if (given.empty()) {
      return 0;
    }
    return oneGray ? given[0] : given[at % given.size()];
  };
  const bool allowed = oldWidth >= 1 && oldHeight >= 1 && newWidth >= 1 && newHeight >= 1 &&
                       oldWidth <= Scaler::kMaxSide && oldHeight <= Scaler::kMaxSide && newWidth <= Scaler::kMaxSide &&
                       newHeight <= Scaler::kMaxSide;
  // The work is the old picture and the new together: large pictures are
  // left to the unit tests.
  if (allowed && (std::uint64_t{oldWidth} * oldHeight > 4000000 || std::uint64_t{newWidth} * newHeight > 4000000)) {
    return 0;
  }

  std::vector<std::uint32_t> sums(Scaler::SumsFor(std::min(newWidth, Scaler::kMaxSide)), 0xA5A5A5A5U);
  std::vector<std::uint8_t> row(std::min(newWidth, Scaler::kMaxSide), 0xA5);
  Scaler scaler(sums, row);
  Rows rows;
  rows.width = newWidth;
  const std::size_t newPixels = std::size_t{newWidth} * newHeight;
  rows.keep = newPixels <= kMostCompared && std::size_t{oldWidth} * oldHeight <= kMostCompared;
  const ScaleStatus begun = scaler.Begin(oldWidth, oldHeight, newWidth, newHeight, rows);
  if (!allowed) {
    BN_FUZZ_CHECK(begun == ScaleStatus::kBadSize, "a side of nothing or too long was taken");
  }
  std::vector<std::uint8_t> oldRow(allowed ? oldWidth : 0);
  // How many new rows had been given when a row out of turn stopped it.
  std::uint32_t stoppedAt = 0;
  for (std::uint32_t y = 0; allowed && y < oldHeight; ++y) {
    for (std::uint32_t x = 0; x < oldWidth; ++x) {
      oldRow[x] = pixel((std::size_t{y} * oldWidth) + x);
    }
    scaler.Row(y, oldRow);
    if (outOfTurn && y == outOfTurnAt % oldHeight && scaler.Ready() && y + 1 < oldHeight) {
      // Another row before the new ones are given out.
      scaler.Row(y + 1, oldRow);
      BN_FUZZ_CHECK(scaler.Status() == ScaleStatus::kOutOfTurn, "a row out of turn was taken");
      stoppedAt = rows.count;
    }
    rows.inStep = true;
    while (true) {
      const std::uint32_t before = rows.count;
      const bool gave = scaler.Step();
      BN_FUZZ_CHECK(rows.count == before + (gave ? 1U : 0U), "a step gave out other than it said");
      if (!gave) {
        break;
      }
    }
    rows.inStep = false;
  }
  if (begun != ScaleStatus::kOk) {
    BN_FUZZ_CHECK(rows.count == 0 && !scaler.Finished(), "a scaler that did not begin gave something");
    return 0;
  }
  if (scaler.Status() == ScaleStatus::kOutOfTurn) {
    BN_FUZZ_CHECK(!scaler.Finished() && rows.count == stoppedAt, "a scaler that stopped went on");
    return 0;
  }
  BN_FUZZ_CHECK(scaler.Status() == ScaleStatus::kOk && scaler.Finished() && rows.count == newHeight,
                "the new picture was not given out whole");
  if (!rows.keep) {
    return 0;
  }

  // The working-out in real numbers: across, then down.
  std::vector<std::vector<double>> across;
  for (std::uint32_t y = 0; y < oldHeight; ++y) {
    std::vector<double> line(oldWidth);
    for (std::uint32_t x = 0; x < oldWidth; ++x) {
      line[x] = pixel((std::size_t{y} * oldWidth) + x);
    }
    across.push_back(Line(line, newWidth));
  }
  for (std::uint32_t x = 0; x < newWidth; ++x) {
    std::vector<double> column(oldHeight);
    for (std::uint32_t y = 0; y < oldHeight; ++y) {
      column[y] = across[y][x];
    }
    const std::vector<double> down = Line(column, newHeight);
    for (std::uint32_t y = 0; y < newHeight; ++y) {
      const std::uint8_t made = rows.pixels[(std::size_t{y} * newWidth) + x];
      BN_FUZZ_CHECK(std::abs(made - down[y]) <= 1.0, "a new pixel is more than a gray from where it should be");
      if (oneGray) {
        BN_FUZZ_CHECK(made == pixel(0), "a picture of one gray did not stay that gray");
      }
    }
  }
  return 0;
}
