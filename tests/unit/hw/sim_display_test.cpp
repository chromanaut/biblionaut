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

bool RowIsBlackOfWidth(std::span<const std::uint8_t> picture, std::size_t rowBytes, std::size_t row) {
  const std::span<const std::uint8_t> bytes = picture.subspan(row * rowBytes, rowBytes);
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0x00; });
}

bool RowIsWhite(std::span<const std::uint8_t> picture, std::size_t row) {
  const std::span<const std::uint8_t> bytes = picture.subspan(row * kRowBytes, kRowBytes);
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0xFF; });
}

struct SimFixture {
  bn::test::FakeClock clock;
  SimDisplay display{clock, bn::hw::X4(), ""};

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

  fixture.clock.now += bn::hw::X4().times.fullRefreshUs - 1;
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

TEST(SimDisplay, RefusesTheGrayPassAsAPanelThatShowsNoGrays) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> picture = WithBlackRows(100, 50);
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kHalf, AfterRefresh::kPowerOff));
  fixture.Finish();

  EXPECT_FALSE(fixture.display.StartGrayRefresh(AfterRefresh::kPowerOff));
  EXPECT_FALSE(fixture.display.Busy());
  EXPECT_EQ(fixture.display.RefreshCount(), 1);
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 100));
}

TEST(SimDisplay, IsTheSizeOfItsDeviceAndTakesAsLongAsItsPanelDoes) {
  const bn::hw::DeviceProfile device{
      .name = "other",
      .panel = {.width = 400, .height = 300},
      .times = {.fullRefreshUs = 2'000, .halfRefreshUs = 1'000, .fastRefreshUs = 300, .powerOffUs = 70},
      .buttons = {}};
  bn::test::FakeClock clock;
  SimDisplay display(clock, device, "");
  EXPECT_EQ(&display.Device(), &device);
  EXPECT_EQ(display.Size().width, 400u);
  EXPECT_EQ(display.Size().height, 300u);
  ASSERT_TRUE(display.Start());
  EXPECT_EQ(display.Shown().size(), 15'000u);  // 300 rows of 50 bytes.

  // Rows of its width, as many as it has, and no more.
  const std::vector<std::uint8_t> rows(15'000, 0x00);
  EXPECT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, rows));
  EXPECT_FALSE(display.WriteRows(DisplayPlane::kNew, 1, rows));

  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  clock.now = 1'999;
  EXPECT_TRUE(display.Busy());
  clock.now = 2'000;
  EXPECT_FALSE(display.Busy());
  EXPECT_TRUE(RowIsBlackOfWidth(display.Shown(), 50, 299));

  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  EXPECT_EQ(display.LastRefresh().doneUs, 2'300u);
  clock.now = 2'369;
  EXPECT_TRUE(display.Busy());
  clock.now = 2'370;
  EXPECT_FALSE(display.Busy());
}

TEST(SimDisplay, RemembersTheLastRefreshAndWhatWasShownBeforeIt) {
  bn::test::FakeClock clock;
  SimDisplay display(clock, bn::hw::X4(), "", 4);  // Four times quicker than a real panel.
  ASSERT_TRUE(display.Start());
  EXPECT_EQ(display.LastRefresh().doneUs, 0u);
  EXPECT_TRUE(RowIsWhite(display.ShownBefore(), 0));

  clock.now = 1'000;
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, WithBlackRows(10, 1)));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kHalf, AfterRefresh::kStayPowered));
  EXPECT_EQ(display.LastRefresh().mode, RefreshMode::kHalf);
  EXPECT_EQ(display.LastRefresh().startUs, 1'000u);
  EXPECT_EQ(display.LastRefresh().doneUs, 1'000 + (bn::hw::X4().times.halfRefreshUs / 4));
  EXPECT_TRUE(RowIsWhite(display.ShownBefore(), 10));
  EXPECT_TRUE(RowIsBlack(display.Shown(), 10));

  // Switching off after a refresh keeps the panel busy longer, and the
  // picture is finished when the refresh is.
  clock.now = 2'000'000;
  ASSERT_FALSE(display.Busy());
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, WithBlackRows(20, 1)));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  EXPECT_EQ(display.LastRefresh().mode, RefreshMode::kFast);
  EXPECT_EQ(display.LastRefresh().doneUs, 2'000'000 + (bn::hw::X4().times.fastRefreshUs / 4));
  EXPECT_TRUE(RowIsBlack(display.ShownBefore(), 10));
  EXPECT_TRUE(RowIsWhite(display.ShownBefore(), 20));
  clock.now = display.LastRefresh().doneUs;
  EXPECT_TRUE(display.Busy());
}

TEST(SimDisplay, AFastRefreshLeavesAloneOnlyWhatBothPlanesCallWhite) {
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
  fixture.clock.now += bn::hw::X4().times.fastRefreshUs;
  EXPECT_FALSE(fixture.display.Busy());
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 320));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 120)) << "left over from the first picture";

  // With the old-image plane holding what the panel shows, the same change
  // comes out right.
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, first));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, first));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kHalf, AfterRefresh::kStayPowered));
  fixture.clock.now += bn::hw::X4().times.halfRefreshUs;
  ASSERT_FALSE(fixture.display.Busy());
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, second));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  fixture.Finish();
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 120));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 320));
  EXPECT_FALSE(fixture.display.Powered());
  EXPECT_EQ(fixture.display.RefreshCount(), 4);
}

TEST(SimDisplay, AFastRefreshDrivesBlackWhateverTheOldPlaneSays) {
  // Both planes say rows 100 to 149 are black, but the panel is white: a
  // wrong old-image plane costs nothing where the new image is black.
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> picture = WithBlackRows(100, 50);
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, picture));
  ASSERT_TRUE(RowIsWhite(fixture.display.Shown(), 120));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  fixture.Finish();
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 120));
  EXPECT_TRUE(RowIsWhite(fixture.display.Shown(), 99));
}

// Shows `picture` by a fast refresh, writing only the new-image plane.
void StepTo(SimFixture& fixture, const std::vector<std::uint8_t>& picture) {
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  fixture.Finish();
}

// Shows `picture` by a full refresh, with both planes holding it.
void StartFrom(SimFixture& fixture, const std::vector<std::uint8_t>& picture) {
  ASSERT_TRUE(fixture.display.Start());
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, picture));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  fixture.Finish();
}

bool Same(std::span<const std::uint8_t> a, const std::vector<std::uint8_t>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

TEST(SimDisplay, AfterAFastRefreshThePlanesHaveChangedPlaces) {
  SimFixture fixture;
  const std::vector<std::uint8_t> first = WithBlackRows(100, 50);
  const std::vector<std::uint8_t> second = WithBlackRows(200, 50);
  const std::vector<std::uint8_t> third = WithBlackRows(300, 50);
  StartFrom(fixture, first);
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), first)) << "a full refresh moves nothing";

  // What was the new image is now the old one, with nothing written; the
  // new-image plane is left holding the picture before.
  StepTo(fixture, second);
  EXPECT_TRUE(Same(fixture.display.Shown(), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), first));
  // Reading says the same.
  std::vector<std::uint8_t> rows(kRowBytes * kRows);
  ASSERT_TRUE(fixture.display.ReadRows(DisplayPlane::kOld, 0, rows));
  EXPECT_EQ(rows, second);

  // So fast refreshes can follow one another with only the new-image
  // plane ever written.
  StepTo(fixture, third);
  EXPECT_TRUE(Same(fixture.display.Shown(), third));
  StepTo(fixture, first);
  EXPECT_TRUE(Same(fixture.display.Shown(), first));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), first));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), third));
}

TEST(SimDisplay, SwitchingOffPutsThePlanesBackInTheirFirstPlaces) {
  SimFixture fixture;
  const std::vector<std::uint8_t> first = WithBlackRows(100, 50);
  const std::vector<std::uint8_t> second = WithBlackRows(200, 50);
  const std::vector<std::uint8_t> third = WithBlackRows(300, 50);

  // After one fast refresh that undoes the change of places: the
  // old-image plane is a picture behind the panel.
  StartFrom(fixture, first);
  StepTo(fixture, second);
  ASSERT_TRUE(fixture.display.PowerOff());
  fixture.Finish();
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), first));
  // A fast refresh now compares against the wrong picture. Rows 200 to
  // 249, black on the panel, are white in both planes and are left black.
  StepTo(fixture, third);
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 320));
  EXPECT_TRUE(RowIsBlack(fixture.display.Shown(), 220)) << "left over";

  // Writing the old-image plane with what the panel shows, after the
  // switch-off, is what puts it right.
  StartFrom(fixture, first);
  StepTo(fixture, second);
  ASSERT_TRUE(fixture.display.PowerOff());
  fixture.Finish();
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, second));
  StepTo(fixture, third);
  EXPECT_TRUE(Same(fixture.display.Shown(), third));

  // After two fast refreshes the planes are in their first places already,
  // and switching off changes nothing.
  StartFrom(fixture, first);
  StepTo(fixture, second);
  StepTo(fixture, third);
  ASSERT_TRUE(fixture.display.PowerOff());
  fixture.Finish();
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), third));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), second));
  StepTo(fixture, first);
  EXPECT_TRUE(Same(fixture.display.Shown(), first));

  // A switch-off made as part of the refresh does the same as one made
  // after it.
  StartFrom(fixture, first);
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, second));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kPowerOff));
  fixture.Finish();
  EXPECT_TRUE(Same(fixture.display.Shown(), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), first));

  // Setting the panel up again starts from the beginning.
  StartFrom(fixture, first);
  StepTo(fixture, second);
  ASSERT_TRUE(fixture.display.Start());
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, third));
  StepTo(fixture, third);
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kOld), third));
}

TEST(SimDisplay, TakesNothingWhileItIsBusy) {
  SimFixture fixture;
  const std::vector<std::uint8_t> first = WithBlackRows(100, 50);
  const std::vector<std::uint8_t> second = WithBlackRows(300, 50);
  StartFrom(fixture, first);
  std::vector<std::uint8_t> rows(kRowBytes * 80);

  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, second));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kOld, 0, second));
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kNew, 0, first));
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kNew, 0, rows));
  fixture.Finish();
  EXPECT_TRUE(Same(fixture.display.Shown(), second));
  EXPECT_TRUE(Same(fixture.display.Plane(DisplayPlane::kNew), first)) << "nothing refused left a mark";

  // Nor while the voltages are being switched off.
  ASSERT_TRUE(fixture.display.PowerOff());
  EXPECT_FALSE(fixture.display.WriteRows(DisplayPlane::kOld, 0, second));
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kOld, 0, rows));
  fixture.Finish();
  EXPECT_TRUE(fixture.display.WriteRows(DisplayPlane::kOld, 0, second));
}

TEST(SimDisplay, ReadsBackWhatAPlaneHolds) {
  SimFixture fixture;
  std::vector<std::uint8_t> rows(kRowBytes * 80);
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kNew, 0, rows)) << "before it is started";
  ASSERT_TRUE(fixture.display.Start());

  const std::vector<std::uint8_t> picture = WithBlackRows(100, 50);
  ASSERT_TRUE(fixture.display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(fixture.display.ReadRows(DisplayPlane::kNew, 80, rows));
  EXPECT_TRUE(std::equal(rows.begin(), rows.end(), picture.begin() + static_cast<std::ptrdiff_t>(80 * kRowBytes)));
  ASSERT_TRUE(fixture.display.ReadRows(DisplayPlane::kOld, 80, rows));
  EXPECT_EQ(rows, std::vector<std::uint8_t>(rows.size(), 0xFF)) << "the other plane is still blank";

  // Not rows that do not fit, and not while the panel is busy.
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kNew, 401, rows));
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kNew, 0, std::span(rows).first(150)));
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  EXPECT_FALSE(fixture.display.ReadRows(DisplayPlane::kNew, 80, rows));
  fixture.Finish();
  EXPECT_TRUE(fixture.display.ReadRows(DisplayPlane::kNew, 80, rows));
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

TEST(SimDisplay, WritesAWindowAndLeavesTheRestOfThePlaneAsItWas) {
  SimFixture fixture;
  const std::vector<std::uint8_t> column(kRows, 0x5A);
  const bn::core::DisplayWindow wholeColumn = {.firstRow = 0, .rows = kRows, .firstByte = 3, .bytes = 1};
  EXPECT_FALSE(fixture.display.WriteWindow(DisplayPlane::kNew, wholeColumn, column)) << "before it is started";
  ASSERT_TRUE(fixture.display.Start());

  // Eight columns of pixels the panel's height, as a picture's rows go into
  // a panel turned a quarter, and a block of three bytes by two rows.
  ASSERT_TRUE(fixture.display.WriteWindow(DisplayPlane::kNew, wholeColumn, column));
  const std::vector<std::uint8_t> block = {1, 2, 3, 4, 5, 6};
  ASSERT_TRUE(fixture.display.WriteWindow(DisplayPlane::kOld, {.firstRow = 478, .rows = 2, .firstByte = 97, .bytes = 3},
                                          block));
  const std::span<const std::uint8_t> written = fixture.display.Plane(DisplayPlane::kNew);
  const std::span<const std::uint8_t> other = fixture.display.Plane(DisplayPlane::kOld);
  for (std::size_t row = 0; row < kRows; ++row) {
    for (std::size_t byte = 0; byte < kRowBytes; ++byte) {
      const std::uint8_t newWanted = byte == 3 ? 0x5A : 0xFF;
      ASSERT_EQ(written[(row * kRowBytes) + byte], newWanted) << row << " " << byte;
      const bool inBlock = row >= 478 && byte >= 97;
      const std::uint8_t oldWanted =
          inBlock ? static_cast<std::uint8_t>(1 + ((row - 478) * 3) + (byte - 97)) : std::uint8_t{0xFF};
      ASSERT_EQ(other[(row * kRowBytes) + byte], oldWanted) << row << " " << byte;
    }
  }

  // Not while the panel is busy.
  ASSERT_TRUE(fixture.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  EXPECT_FALSE(fixture.display.WriteWindow(DisplayPlane::kNew, wholeColumn, column));
  fixture.Finish();
  EXPECT_TRUE(fixture.display.WriteWindow(DisplayPlane::kNew, wholeColumn, column));
}

TEST(SimDisplay, RefusesAWindowThatDoesNotFit) {
  SimFixture fixture;
  ASSERT_TRUE(fixture.display.Start());
  const std::vector<std::uint8_t> data(4, 0x00);
  const auto refused = [&fixture, &data](bn::core::DisplayWindow window, std::size_t bytes) {
    return !fixture.display.WriteWindow(DisplayPlane::kNew, window, std::span(data).first(bytes));
  };
  EXPECT_TRUE(refused({.firstRow = 0, .rows = 0, .firstByte = 0, .bytes = 1}, 0)) << "no rows";
  EXPECT_TRUE(refused({.firstRow = 0, .rows = 1, .firstByte = 0, .bytes = 0}, 0)) << "no bytes";
  EXPECT_TRUE(refused({.firstRow = 479, .rows = 2, .firstByte = 0, .bytes = 1}, 2)) << "past the last row";
  EXPECT_TRUE(refused({.firstRow = 0, .rows = 1, .firstByte = 99, .bytes = 2}, 2)) << "past the end of a row";
  EXPECT_TRUE(refused({.firstRow = 0, .rows = 2, .firstByte = 0, .bytes = 2}, 3)) << "too few bytes";
  EXPECT_TRUE(refused({.firstRow = 0, .rows = 1, .firstByte = 0, .bytes = 2}, 4)) << "too many bytes";
  EXPECT_FALSE(refused({.firstRow = 479, .rows = 1, .firstByte = 98, .bytes = 2}, 2)) << "the last corner";
  // Nothing refused left a mark.
  const std::span<const std::uint8_t> plane = fixture.display.Plane(DisplayPlane::kNew);
  EXPECT_EQ(std::count(plane.begin(), plane.end(), 0x00), 2);
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
  SimDisplay display(trace.clock, bn::hw::X4(), "");
  ASSERT_TRUE(display.Start());
  trace.clock.now = 1'000;
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  trace.clock.now += bn::hw::X4().times.fastRefreshUs + 250;  // Noticed a little late, as by a main loop.
  EXPECT_FALSE(display.Busy());
  EXPECT_FALSE(display.Busy());

  std::array<bn::core::TraceRecord, 16> records{};
  ASSERT_EQ(trace.tracer.Drain(records), 1u);
  EXPECT_EQ(records[0].id, bn::hw::kSpanRefreshFast);
  EXPECT_EQ(records[0].startUs, 1'000u);
  EXPECT_EQ(records[0].durationUs, bn::hw::X4().times.fastRefreshUs + 250);
  EXPECT_EQ(records[0].arg, 0u);

  // Switching off as part of a refresh makes the refresh that much longer.
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kHalf, AfterRefresh::kPowerOff));
  trace.clock.now += bn::hw::X4().times.halfRefreshUs + bn::hw::X4().times.powerOffUs - 1;
  EXPECT_TRUE(display.Busy());
  trace.clock.now += 1;
  EXPECT_FALSE(display.Busy());
  ASSERT_EQ(trace.tracer.Drain(records), 1u);
  EXPECT_EQ(records[0].id, bn::hw::kSpanRefreshHalf);
  EXPECT_EQ(records[0].arg, 1u);
}

TEST(SimDisplay, SwitchingOffTakesAMomentAndKeepsWhatThePlanesHold) {
  bn::test::TraceFixture<16> trace;
  SimDisplay display(trace.clock, bn::hw::X4(), "");
  EXPECT_FALSE(display.PowerOff()) << "not started";
  ASSERT_TRUE(display.Start());

  // Nothing to switch off yet: done at once.
  EXPECT_TRUE(display.PowerOff());
  EXPECT_FALSE(display.Busy());

  const std::vector<std::uint8_t> picture = WithBlackRows(100, 50);
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, picture));
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kOld, 0, picture));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  EXPECT_FALSE(display.PowerOff()) << "busy";
  trace.clock.now += bn::hw::X4().times.fullRefreshUs;
  ASSERT_FALSE(display.Busy());
  ASSERT_TRUE(display.Powered());

  trace.clock.now = 20'000'000;
  EXPECT_TRUE(display.PowerOff());
  EXPECT_FALSE(display.Powered());
  EXPECT_TRUE(display.Busy());
  trace.clock.now += bn::hw::X4().times.powerOffUs;
  EXPECT_FALSE(display.Busy());
  EXPECT_TRUE(display.Started());
  EXPECT_TRUE(RowIsBlack(display.Plane(DisplayPlane::kNew), 120));
  EXPECT_TRUE(RowIsBlack(display.Plane(DisplayPlane::kOld), 120));

  std::array<bn::core::TraceRecord, 16> records{};
  const std::size_t count = trace.tracer.Drain(records);
  ASSERT_GE(count, 1u);
  EXPECT_EQ(records[count - 1].id, bn::hw::kSpanPanelPowerOff);
  EXPECT_EQ(records[count - 1].startUs, 20'000'000u);
  EXPECT_EQ(records[count - 1].durationUs, bn::hw::X4().times.powerOffUs);

  // The next refresh switches it on again.
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  EXPECT_TRUE(display.Powered());
}

TEST(SimDisplay, CanWorkFasterThanARealPanel) {
  bn::test::FakeClock clock;
  SimDisplay display(clock, bn::hw::X4(), "", 20);
  ASSERT_TRUE(display.Start());
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  clock.now += (bn::hw::X4().times.fullRefreshUs / 20) - 1;
  EXPECT_TRUE(display.Busy());
  clock.now += 1;
  EXPECT_FALSE(display.Busy());

  // A speed of nothing is no speed-up at all.
  SimDisplay ordinary(clock, bn::hw::X4(), "", 0);
  ASSERT_TRUE(ordinary.Start());
  ASSERT_TRUE(ordinary.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));
  clock.now += bn::hw::X4().times.fastRefreshUs - 1;
  EXPECT_TRUE(ordinary.Busy());
}

TEST(SimDisplay, SavesThePanelsPictureAtEveryRefresh) {
  const std::string prefix = testing::TempDir() + "bn_sim_display_test";
  for (const char* suffix : {"-001.png", "-002.png"}) {
    std::remove((prefix + suffix).c_str());
  }
  bn::test::FakeClock clock;
  SimDisplay display(clock, bn::hw::X4(), prefix);
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
