#include "desktop/panel_view.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "trace_fakes.h"

namespace {

using bn::core::AfterRefresh;
using bn::core::DisplayPlane;
using bn::core::RefreshMode;
using bn::hw::PanelView;
using bn::hw::ReadingTurn;
using bn::hw::RefreshStage;
using bn::hw::SimDisplay;
using bn::hw::StageAt;
using bn::hw::ViewTurn;

constexpr std::size_t kWidth = 800;
constexpr std::size_t kHeight = 480;
constexpr std::size_t kRowBytes = 100;

struct ViewFixture {
  bn::test::FakeClock clock;
  SimDisplay display{clock, bn::hw::X4(), ""};

  ViewFixture() { EXPECT_TRUE(display.Start()); }

  // Shows a white picture with black pixels at `blacks` (x, y on the panel),
  // by a refresh of `mode`, and waits for it to be over.
  void Show(const std::vector<std::pair<std::size_t, std::size_t>>& blacks, RefreshMode mode = RefreshMode::kFull) {
    std::vector<std::uint8_t> rows(kRowBytes * kHeight, 0xFF);
    for (const auto& [x, y] : blacks) {
      rows[(y * kRowBytes) + (x / 8)] &= static_cast<std::uint8_t>(~(0x80U >> (x % 8)));
    }
    ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, rows));
    ASSERT_TRUE(display.StartRefresh(mode, AfterRefresh::kStayPowered));
    clock.now += 10'000'000;
    ASSERT_FALSE(display.Busy());
  }
};

std::size_t CountBlack(const PanelView& view) {
  std::size_t count = 0;
  for (const std::uint32_t pixel : view.Pixels()) {
    count += pixel == PanelView::kBlack ? 1 : 0;
  }
  return count;
}

TEST(PanelView, ShowsThePanelTurnedTheWayABookIsRead) {
  ViewFixture f;
  // The first pixel of the first row, and the last of the last.
  f.Show({{0, 0}, {kWidth - 1, kHeight - 1}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kClockwise);
  EXPECT_EQ(view.Turn(), ViewTurn::kAsRead);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  ASSERT_EQ(view.Size().width, kHeight);
  ASSERT_EQ(view.Size().height, kWidth);
  ASSERT_EQ(view.Pixels().size(), kWidth * kHeight);
  EXPECT_EQ(CountBlack(view), 2u);
  // A quarter-turn clockwise: the panel's first row runs down the right of
  // the window, and its last row down the left.
  EXPECT_EQ(view.Pixels()[kHeight - 1], PanelView::kBlack);             // Top right.
  EXPECT_EQ(view.Pixels()[(kWidth - 1) * kHeight], PanelView::kBlack);  // Bottom left.
  EXPECT_EQ(view.Pixels()[0], PanelView::kWhite);                       // Top left.
  EXPECT_EQ(view.Pixels()[(kWidth * kHeight) - 1], PanelView::kWhite);  // Bottom right.
}

TEST(PanelView, TurnsThePanelWhicheverWayTheDeviceIsRead) {
  // A small panel with the first two pixels of its first row black: where
  // they land says which way it was turned.
  constexpr bn::core::DisplaySize kSmall{.width = 16, .height = 8};
  const bn::hw::DeviceProfile device{
      .name = "small",
      .panel = kSmall,
      .times = {.fullRefreshUs = 10, .halfRefreshUs = 10, .fastRefreshUs = 10, .powerOffUs = 10},
      .buttons = {}};
  bn::test::FakeClock clock;
  SimDisplay display(clock, device, "");
  ASSERT_TRUE(display.Start());
  std::vector<std::uint8_t> rows(kSmall.RowBytes() * kSmall.height, 0xFF);
  rows[0] = 0x3F;
  ASSERT_TRUE(display.WriteRows(DisplayPlane::kNew, 0, rows));
  ASSERT_TRUE(display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  clock.now = 1'000;

  struct Expected {
    ReadingTurn turn;
    std::size_t width;
    std::size_t firstAt;   // Where the first pixel goes, as an index into Pixels().
    std::size_t secondAt;  // And the one after it in the row.
  };
  const Expected expected[] = {
      // As it is: the row runs along the top from the left.
      {.turn = ReadingTurn::kNone, .width = 16, .firstAt = 0, .secondAt = 1},
      // A quarter-turn clockwise: down the right edge from the top.
      {.turn = ReadingTurn::kClockwise, .width = 8, .firstAt = 7, .secondAt = (1 * 8) + 7},
      // Upside down: along the bottom from the right.
      {.turn = ReadingTurn::kHalf, .width = 16, .firstAt = (7 * 16) + 15, .secondAt = (7 * 16) + 14},
      // A quarter-turn anticlockwise: up the left edge from the bottom.
      // (The last row of 8, and the one above it.)
      {.turn = ReadingTurn::kAnticlockwise, .width = 8, .firstAt = 120, .secondAt = 112},
  };
  for (const Expected& e : expected) {
    PanelView view(kSmall, e.turn);
    ASSERT_TRUE(view.Update(display, clock.now));
    EXPECT_EQ(view.Size().width, e.width) << static_cast<int>(e.turn);
    EXPECT_EQ(static_cast<std::size_t>(view.Size().width) * view.Size().height, kSmall.width * kSmall.height);
    EXPECT_EQ(view.Pixels()[e.firstAt], PanelView::kBlack) << static_cast<int>(e.turn);
    EXPECT_EQ(view.Pixels()[e.secondAt], PanelView::kBlack) << static_cast<int>(e.turn);
    std::size_t black = 0;
    for (const std::uint32_t pixel : view.Pixels()) {
      black += pixel == PanelView::kBlack ? 1 : 0;
    }
    EXPECT_EQ(black, 2u) << static_cast<int>(e.turn);

    // In the panel's own way, however the device is read, nothing is turned.
    view.SetTurn(ViewTurn::kAsPanel);
    ASSERT_TRUE(view.Update(display, clock.now));
    EXPECT_EQ(view.Size().width, kSmall.width);
    EXPECT_EQ(view.Pixels()[0], PanelView::kBlack);
    EXPECT_EQ(view.Pixels()[1], PanelView::kBlack);
  }
}

TEST(PanelView, ShowsThePanelInItsOwnOrientationWhenTurned) {
  ViewFixture f;
  f.Show({{3, 0}, {kWidth - 1, 2}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kClockwise);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  view.SetTurn(ViewTurn::kAsPanel);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));
  ASSERT_EQ(view.Size().width, kWidth);
  ASSERT_EQ(view.Size().height, kHeight);
  EXPECT_EQ(CountBlack(view), 2u);
  EXPECT_EQ(view.Pixels()[3], PanelView::kBlack);
  EXPECT_EQ(view.Pixels()[(2 * kWidth) + kWidth - 1], PanelView::kBlack);

  // And back.
  view.SetTurn(ViewTurn::kAsRead);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_EQ(view.Size().width, kHeight);
  EXPECT_EQ(view.Pixels()[(3 * kHeight) + kHeight - 1], PanelView::kBlack);
}

TEST(PanelView, FlashesThroughAFullRefreshAndEndsOnTheNewPicture) {
  ViewFixture f;
  f.Show({{10, 10}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kClockwise);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  // A picture with no black at all, by a full refresh.
  const std::uint64_t start = f.clock.now;
  std::vector<std::uint8_t> white(kRowBytes * kHeight, 0xFF);
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kNew, 0, white));
  ASSERT_TRUE(f.display.StartRefresh(RefreshMode::kFull, AfterRefresh::kStayPowered));
  const std::uint64_t length = bn::hw::X4().times.fullRefreshUs;
  EXPECT_EQ(f.display.LastRefresh().doneUs, start + length);

  // The old picture, black for white.
  ASSERT_TRUE(view.Update(f.display, start));
  EXPECT_EQ(view.Stage(), RefreshStage::kInverted);
  EXPECT_EQ(CountBlack(view), (kWidth * kHeight) - 1);
  // Then all black, then all white.
  ASSERT_TRUE(view.Update(f.display, start + (length / 2)));
  EXPECT_EQ(view.Stage(), RefreshStage::kBlack);
  EXPECT_EQ(CountBlack(view), kWidth * kHeight);
  ASSERT_TRUE(view.Update(f.display, start + length - 1));
  EXPECT_EQ(view.Stage(), RefreshStage::kWhite);
  EXPECT_EQ(CountBlack(view), 0u);
  // Then the new picture, once the refresh is over.
  ASSERT_TRUE(view.Update(f.display, start + length));
  EXPECT_EQ(view.Stage(), RefreshStage::kAfter);
  EXPECT_EQ(CountBlack(view), 0u);
}

TEST(PanelView, ShowsAFastRefreshsPictureOnceItIsOver) {
  ViewFixture f;
  f.Show({{10, 10}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kClockwise);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));

  const std::uint64_t start = f.clock.now;
  std::vector<std::uint8_t> rows(kRowBytes * kHeight, 0xFF);
  rows[(20 * kRowBytes) + 2] = 0x00;  // Eight black pixels.
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kNew, 0, rows));
  ASSERT_TRUE(f.display.StartRefresh(RefreshMode::kFast, AfterRefresh::kStayPowered));

  // Until it is over, the picture from before.
  ASSERT_TRUE(view.Update(f.display, start));
  EXPECT_EQ(view.Stage(), RefreshStage::kBefore);
  EXPECT_EQ(CountBlack(view), 1u);
  EXPECT_FALSE(view.Update(f.display, start + bn::hw::X4().times.fastRefreshUs - 1));

  // Then what the fast refresh left: its own pixels and, where the old-image
  // plane was white, the one from before as well.
  ASSERT_TRUE(view.Update(f.display, start + bn::hw::X4().times.fastRefreshUs));
  EXPECT_EQ(view.Stage(), RefreshStage::kAfter);
  EXPECT_EQ(CountBlack(view), 9u);
}

TEST(PanelView, ShowsTheGraysOfAGrayPassOnceItIsOver) {
  ViewFixture f;
  f.Show({{10, 10}, {11, 10}, {12, 10}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kNone);
  ASSERT_TRUE(view.Update(f.display, f.clock.now));
  ASSERT_EQ(CountBlack(view), 3u);

  // The pixel at 10 is marked dark gray, the one at 11 light gray; 12 is
  // left black.
  const std::uint64_t start = f.clock.now;
  std::vector<std::uint8_t> dark(kRowBytes * kHeight, 0x00);
  std::vector<std::uint8_t> either(kRowBytes * kHeight, 0x00);
  dark[(10 * kRowBytes) + 1] = 0x20;
  either[(10 * kRowBytes) + 1] = 0x30;
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kNew, 0, dark));
  ASSERT_TRUE(f.display.WriteRows(DisplayPlane::kOld, 0, either));
  ASSERT_TRUE(f.display.StartGrayRefresh(AfterRefresh::kPowerOff));

  ASSERT_TRUE(view.Update(f.display, start));
  EXPECT_EQ(view.Stage(), RefreshStage::kBefore);
  EXPECT_EQ(CountBlack(view), 3u);

  ASSERT_TRUE(view.Update(f.display, start + bn::hw::X4().times.grayRefreshUs));
  EXPECT_EQ(view.Stage(), RefreshStage::kAfter);
  EXPECT_EQ(view.Pixels()[(10 * kWidth) + 10], 0xFF555555U);
  EXPECT_EQ(view.Pixels()[(10 * kWidth) + 11], 0xFFAAAAAAU);
  EXPECT_EQ(view.Pixels()[(10 * kWidth) + 12], PanelView::kBlack);
  EXPECT_EQ(view.Pixels()[(10 * kWidth) + 13], PanelView::kWhite);
}

TEST(PanelView, ChangesOnlyWhenWhatThePanelLooksLikeDoes) {
  ViewFixture f;
  f.Show({{0, 0}});
  PanelView view({.width = kWidth, .height = kHeight}, ReadingTurn::kClockwise);
  EXPECT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now + 5'000'000));

  // A new refresh, even of the same picture, is a change.
  f.Show({{0, 0}}, RefreshMode::kHalf);
  EXPECT_TRUE(view.Update(f.display, f.clock.now));
  EXPECT_FALSE(view.Update(f.display, f.clock.now));
}

TEST(PanelView, TakesAHalfRefreshThroughBlackAndWhite) {
  const SimDisplay::Refresh half{.mode = RefreshMode::kHalf, .startUs = 1'000, .doneUs = 1'600};
  EXPECT_EQ(StageAt(half, 0), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(half, 1'299), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(half, 1'300), RefreshStage::kWhite);
  EXPECT_EQ(StageAt(half, 1'599), RefreshStage::kWhite);
  EXPECT_EQ(StageAt(half, 1'600), RefreshStage::kAfter);

  const SimDisplay::Refresh full{.mode = RefreshMode::kFull, .startUs = 1'000, .doneUs = 1'600};
  EXPECT_EQ(StageAt(full, 1'199), RefreshStage::kInverted);
  EXPECT_EQ(StageAt(full, 1'200), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(full, 1'399), RefreshStage::kBlack);
  EXPECT_EQ(StageAt(full, 1'400), RefreshStage::kWhite);

  // Before any refresh, the one that was over at 0.
  EXPECT_EQ(StageAt(SimDisplay::Refresh{}, 0), RefreshStage::kAfter);
}

}  // namespace
