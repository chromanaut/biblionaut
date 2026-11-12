#include "desktop/sim_display.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/checksum.h"
#include "core/endian.h"
#include "hw/trace_ids.h"
#include "trace_fakes.h"

namespace {

using bn::core::AfterRefresh;
using bn::core::DisplayPlane;
using bn::core::RefreshMode;
using bn::hw::SimDisplay;

constexpr std::size_t kRowBytes = 100;
constexpr std::size_t kRows = 480;

// A plane's worth of rows: white, with rows [firstBlack, firstBlack + count) black.
std::vector<std::uint8_t> WithBlackRows(std::size_t firstBlack, std::size_t count) {
  std::vector<std::uint8_t> rows(kRowBytes * kRows, 0xFF);
  std::fill_n(rows.begin() + static_cast<std::ptrdiff_t>(firstBlack * kRowBytes), count * kRowBytes, 0x00);
  return rows;
}

bool RowIsBlack(std::span<const std::uint8_t> picture, std::size_t row) {
  const std::span<const std::uint8_t> bytes = picture.subspan(row * kRowBytes, kRowBytes);
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0x00; });
}

bool RowIsWhite(std::span<const std::uint8_t> picture, std::size_t row) {
  const std::span<const std::uint8_t> bytes = picture.subspan(row * kRowBytes, kRowBytes);
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0xFF; });
}

struct SimFixture {
  bn::test::FakeClock clock;
  SimDisplay display{clock, ""};

  void Finish() {
    clock.now += 10'000'000;
    EXPECT_FALSE(display.Busy());
  }
};

std::uint32_t Be32(std::span<const std::uint8_t> bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
}

// Reads a PNG as this code writes them (stored, not compressed) back into
// rows, checking every checksum on the way.
struct DecodedPng {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bitDepth = 0;
  std::uint8_t colourType = 0;
  std::vector<std::uint8_t> rows;
  int storedBlocks = 0;
};

DecodedPng DecodePng(std::span<const std::uint8_t> png) {
  DecodedPng decoded;
  const std::array<std::uint8_t, 8> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_GE(png.size(), signature.size());
  EXPECT_TRUE(std::equal(signature.begin(), signature.end(), png.begin()));

  std::vector<std::uint8_t> zlib;
  std::size_t at = signature.size();
  bool ended = false;
  while (at + 12 <= png.size() && !ended) {
    const std::uint32_t length = Be32(png.subspan(at, 4));
    const std::string type(png.begin() + static_cast<std::ptrdiff_t>(at + 4),
                           png.begin() + static_cast<std::ptrdiff_t>(at + 8));
    const std::span<const std::uint8_t> data = png.subspan(at + 8, length);
    EXPECT_EQ(bn::core::Crc32(png.subspan(at + 4, 4 + length)), Be32(png.subspan(at + 8 + length, 4))) << type;
    if (type == "IHDR") {
      EXPECT_EQ(length, 13u);
      decoded.width = Be32(data.subspan(0, 4));
      decoded.height = Be32(data.subspan(4, 4));
      decoded.bitDepth = data[8];
      decoded.colourType = data[9];
    } else if (type == "IDAT") {
      zlib.insert(zlib.end(), data.begin(), data.end());
    } else if (type == "IEND") {
      ended = true;
    }
    at += 12 + length;
  }
  EXPECT_TRUE(ended);
  EXPECT_EQ(at, png.size());

  // The zlib stream: two header bytes, stored blocks, a checksum.
  EXPECT_GE(zlib.size(), 6u);
  std::vector<std::uint8_t> raw;
  std::size_t in = 2;
  bool last = false;
  while (!last && in + 5 <= zlib.size()) {
    last = (zlib[in] & 1U) != 0;
    EXPECT_EQ(zlib[in] & 6U, 0u) << "not a stored block";
    const std::size_t length = zlib[in + 1] | (static_cast<std::size_t>(zlib[in + 2]) << 8U);
    const std::size_t check = zlib[in + 3] | (static_cast<std::size_t>(zlib[in + 4]) << 8U);
    EXPECT_EQ(length ^ check, 0xFFFFu);
    raw.insert(raw.end(), zlib.begin() + static_cast<std::ptrdiff_t>(in + 5),
               zlib.begin() + static_cast<std::ptrdiff_t>(in + 5 + length));
    in += 5 + length;
    ++decoded.storedBlocks;
  }
  EXPECT_TRUE(last);
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  EXPECT_EQ(in + 4, zlib.size());
  EXPECT_EQ(Be32(std::span(zlib).subspan(in, 4)), (b << 16U) | a);

  // Each row has a filter byte in front, which must say "none".
  const std::size_t rowBytes = (decoded.width + 7) / 8;
  EXPECT_EQ(raw.size(), decoded.height * (rowBytes + 1));
  for (std::size_t y = 0; y < decoded.height && (y + 1) * (rowBytes + 1) <= raw.size(); ++y) {
    EXPECT_EQ(raw[y * (rowBytes + 1)], 0u);
    decoded.rows.insert(decoded.rows.end(), raw.begin() + static_cast<std::ptrdiff_t>((y * (rowBytes + 1)) + 1),
                        raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * (rowBytes + 1)));
  }
  return decoded;
}

TEST(SimDisplay, IsThePanelsSizeAndDoesNothingUntilStarted) {
  SimFixture fixture;
  EXPECT_EQ(fixture.display.Size().width, 800u);
  EXPECT_EQ(fixture.display.Size().height, 480u);
  EXPECT_EQ(fixture.display.Size().RowBytes(), kRowBytes);

  const std::vector<std::uint8_t> rows = WithBlackRows(0, 480);
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, rows));
  EXPECT_FALSE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  EXPECT_FALSE(fixture.display.Busy());
  EXPECT_TRUE(fixture.display.Sleep());

  EXPECT_TRUE(fixture.display.Start());
  EXPECT_TRUE(fixture.display.Started());
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 0));
}

TEST(SimDisplay, AFullRefreshShowsTheNewPlaneAndKeepsThePanelBusyMeanwhile) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> picture = WithBlackRows(100, 50);
  // Rows can arrive in any pieces, in any order.
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 80, std::span(picture).subspan(80 * kRowBytes)));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, std::span(picture).first(80 * kRowBytes)));
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 120));  // Nothing shows until a refresh.

  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  EXPECT_TRUE(fixture.display.Busy());
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  EXPECT_FALSE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  EXPECT_FALSE(fixture.display.Sleep());

  fixture.clock.now += SimDisplay::kFullRefreshUs - 1;
  EXPECT_TRUE(fixture.display.Busy());
  fixture.clock.now += 1;
  EXPECT_FALSE(fixture.display.Busy());

  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 99));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 100));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 149));
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 150));
  EXPECT_EQ(fixture.display.RefreshCount(), 1);
  EXPECT_TRUE(fixture.display.Powered());
}

TEST(SimDisplay, AFastRefreshOnlyChangesPixelsWhereThePlanesDiffer) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> first = WithBlackRows(100, 50);
  const std::vector<std::uint8_t> second = WithBlackRows(300, 50);

  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, first));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  fixture.Finish();

  // The old-image plane was never told about the first picture: it still
  // says the panel is white. A fast refresh to the second picture therefore
  // sees no reason to touch rows 100 to 149, and they stay black.
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, second));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  fixture.clock.now += SimDisplay::kFastRefreshUs;
  EXPECT_FALSE(fixture.display.Busy());
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 320));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 120)) << "left over from the first picture";

  // With the old-image plane kept in step, the same change comes out right.
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, first));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, first));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kHalf, AfterRefresh::kStayPowered));
  fixture.clock.now += SimDisplay::kHalfRefreshUs;
  ASSERT_FALSE(fixture.display.Busy());
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, second));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  fixture.Finish();
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 120));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 320));
  EXPECT_FALSE(fixture.display.Powered());
  EXPECT_EQ(fixture.display.RefreshCount(), 4);
}

TEST(SimDisplay, RefusesRowsThatDoNotFit) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> rows(kRowBytes * 3, 0x00);
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, {}));
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, std::span(rows).first(kRowBytes + 1)));
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 478, rows));
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kOld, 480, std::span(rows).first(kRowBytes)));
  EXPECT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 477, rows));
  // Nothing refused left a mark.
  EXPECT_TRUE(RowIsWhite(fixture.display.Plane(DisplayPlane::kNew), 0));
  EXPECT_TRUE(RowIsWhite(fixture.display.Plane(DisplayPlane::kNew), 479));
  EXPECT_TRUE(RowIsBlack(fixture.display.Plane(DisplayPlane::kOld), 479));
}

TEST(SimDisplay, SleepKeepsThePictureAndStartingAgainClearsThePlanes) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> picture = WithBlackRows(0, 480);
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, picture));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  fixture.Finish();

  EXPECT_TRUE(fixture.display.Sleep());
  EXPECT_FALSE(fixture.display.Started());
  EXPECT_FALSE(fixture.display.Powered());
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 200));

  EXPECT_TRUE(fixture.display.Start());
  EXPECT_TRUE(RowIsWhite(fixture.display.Plane(DisplayPlane::kNew), 200));
  EXPECT_TRUE(RowIsWhite(fixture.display.Plane(DisplayPlane::kOld), 200));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 200)) << "the panel keeps its picture until refreshed";
}

TEST(SimDisplay, StartingAgainAbandonsARefreshInProgress) {
  // As when the program is restarted while the panel is still refreshing.
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  ASSERT_TRUE(fixture.display.Busy());

  EXPECT_TRUE(fixture.display.Start());
  EXPECT_FALSE(fixture.display.Busy());
  EXPECT_FALSE(fixture.display.Powered());
  EXPECT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, WithBlackRows(0, 1)));
}

TEST(SimDisplay, RecordsEachRefreshInTheTraceFromStartToFinish) {
  bn::test::TraceFixture<16> trace;
  SimDisplay display(trace.clock, "");
  ASSERT_TRUE(display.Start());
  trace.clock.now = 1'000;
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  trace.clock.now += SimDisplay::kFastRefreshUs + 250;  // Noticed a little late, as by a main loop.
  EXPECT_FALSE(display.Busy());
  EXPECT_FALSE(display.Busy());

  std::array<bn::core::TraceRecord, 16> records{};
  ASSERT_EQ(trace.tracer.Drain(records), 1u);
  EXPECT_EQ(records[0].id, bn::hw::kSpanRefreshFast);
  EXPECT_EQ(records[0].startUs, 1'000u);
  EXPECT_EQ(records[0].durationUs, SimDisplay::kFastRefreshUs + 250);
  EXPECT_EQ(records[0].arg, 1u);
}

TEST(SimDisplay, SavesThePanelsPictureAtEveryRefresh) {
  const std::string prefix = testing::TempDir() + "bn_sim_display_test";
  for (const char* suffix : {"-001.png", "-002.png"}) {
    std::remove((prefix + suffix).c_str());
  }
  bn::test::FakeClock clock;
  SimDisplay display(clock, prefix);
  ASSERT_TRUE(display.Start());
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, WithBlackRows(10, 5)));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  clock.now += 10'000'000;
  ASSERT_FALSE(display.Busy());
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, WithBlackRows(400, 5)));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));

  for (const auto& [suffix, blackRow] :
       {std::pair{"-001.png", std::size_t{12}}, std::pair{"-002.png", std::size_t{402}}}) {
    std::ifstream file(prefix + suffix, std::ios::binary);
    ASSERT_TRUE(file.good()) << suffix;
    const std::vector<std::uint8_t> png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const DecodedPng decoded = DecodePng(png);
    EXPECT_EQ(decoded.width, 800u);
    EXPECT_EQ(decoded.height, 480u);
    EXPECT_EQ(decoded.bitDepth, 1);
    EXPECT_EQ(decoded.colourType, 0);  // Greyscale, where a set bit is white: the same as a plane.
    ASSERT_EQ(decoded.rows.size(), kRowBytes * kRows);
    EXPECT_TRUE(RowIsBlack(decoded.rows, blackRow)) << suffix;
    EXPECT_TRUE(RowIsWhite(decoded.rows, 200)) << suffix;
    std::remove((prefix + suffix).c_str());
  }
}

TEST(EncodePng, HandlesPicturesTooBigForOneBlock) {
  // 800 x 700 is more than the 65,535 bytes one stored block holds.
  const bn::core::DisplaySize size{.width = 800, .height = 700};
  std::vector<std::uint8_t> rows(size.RowBytes() * size.height);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i] = static_cast<std::uint8_t>(i * 31);
  }
  const DecodedPng decoded = DecodePng(bn::hw::EncodePng(size, rows));
  EXPECT_EQ(decoded.width, 800u);
  EXPECT_EQ(decoded.height, 700u);
  EXPECT_EQ(decoded.storedBlocks, 2);
  EXPECT_EQ(decoded.rows, rows);

  // And an odd width, where a row ends part-way through a byte.
  const bn::core::DisplaySize narrow{.width = 13, .height = 3};
  const std::vector<std::uint8_t> narrowRows = {0xAA, 0xA8, 0x55, 0x50, 0xFF, 0xF8};
  const DecodedPng small = DecodePng(bn::hw::EncodePng(narrow, narrowRows));
  EXPECT_EQ(small.width, 13u);
  EXPECT_EQ(small.rows, narrowRows);
}

}  // namespace
