#include "reader/drawing_viewer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "display_fakes.h"
#include "jpeg_builder.h"
#include "png_builder.h"
#include "storage_fakes.h"
#include "stream_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::InputEvent;
using bn::core::InputEventKind;
using bn::reader::DrawingViewer;
using bn::test::FakeDisplay;
using Op = FakeDisplay::Op;
using State = DrawingViewer::State;

// A clock that moves on a little every time it is read, so that whatever
// is timed takes some time.
class SteppingClock final : public bn::core::Clock {
 public:
  std::uint64_t NowUs() noexcept override {
    now += step;
    return now;
  }
  std::uint64_t now = 0;
  std::uint64_t step = 10;
};

// A drawing `width` by `height` holding `body`.
std::string Svg(int width, int height, const std::string& body) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(width) + "' height='" +
         std::to_string(height) + "'>" + body + "</svg>";
}

struct Fixture {
  Fixture() {
    display.present = true;
    display.Resize({.width = 40, .height = 24});
  }

  void Put(const std::string& name, const std::string& contents) {
    storage.Put("/drawings/" + name, {contents.begin(), contents.end()});
  }
  void Start(bool tour = false) {
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    viewer = std::make_unique<DrawingViewer>(display, clock, storage, stream, DrawingViewer::kFolder);
    if (tour) {
      viewer->Tour();
    }
    viewer->Start();
  }
  // Turns of the loop until the panel is shown something and done with it,
  // and nothing more is to be drawn; the panel finishes what it is given
  // at once. Returns the turns it took.
  int Settle(int most = 100000) {
    // Quiet within this settling: not since the last.
    quietTurns_ = 0;
    lastCalls_ = display.calls.size();
    for (int turn = 0; turn < most; ++turn) {
      display.busy = false;
      const std::size_t calls = display.calls.size();
      viewer->Tick();
      if (display.calls.size() == calls && !display.busy && viewer->CurrentState() != State::kReady) {
        return turn;
      }
      if (display.calls.size() == calls && !display.busy && turn > 2 && Quiet()) {
        return turn;
      }
    }
    ADD_FAILURE() << "never settled";
    return most;
  }
  // Nothing was asked of the panel in the last few turns.
  bool Quiet() {
    quietTurns_ = display.calls.size() == lastCalls_ ? quietTurns_ + 1 : 0;
    lastCalls_ = display.calls.size();
    return quietTurns_ > 50;
  }
  void Press(bn::core::Button button) {
    viewer->OnInput(InputEvent{.timeUs = clock.now, .button = button, .kind = InputEventKind::kPress});
  }
  // Whether the pixel at (x, y) of what the panel shows is black.
  bool Black(std::size_t x, std::size_t y) const {
    const std::size_t rowBytes = display.size.RowBytes();
    return (display.shown[(y * rowBytes) + (x / 8)] & (0x80U >> (x % 8))) == 0;
  }
  std::size_t BlackCount() const {
    std::size_t count = 0;
    for (std::size_t y = 0; y < display.size.height; ++y) {
      for (std::size_t x = 0; x < display.size.width; ++x) {
        count += Black(x, y) ? 1 : 0;
      }
    }
    return count;
  }
  // The lines of log text the viewer sent: those that say what each
  // drawing came to, those that say where its time went, or those that
  // say what the cache did.
  enum class Kind : std::uint8_t { kWhat, kTimed, kCache };
  std::vector<std::string> LinesOf(Kind kind) const {
    std::vector<std::string> lines;
    for (const bn::test::StreamItem& item : bn::test::ParseStream(sink.received)) {
      if (!item.isFrame || item.type != bn::core::FrameType::kText) {
        continue;
      }
      const std::string text = item.Text();
      const Kind found = text.find(" timed: ") != std::string::npos   ? Kind::kTimed
                         : text.find(" cache: ") != std::string::npos ? Kind::kCache
                                                                      : Kind::kWhat;
      if (found == kind) {
        lines.push_back(text);
      }
    }
    return lines;
  }
  std::vector<std::string> Lines(bool timed = false) const { return LinesOf(timed ? Kind::kTimed : Kind::kWhat); }
  std::vector<std::string> CacheLines() const { return LinesOf(Kind::kCache); }
  std::vector<bn::core::RefreshMode> Refreshes() const {
    std::vector<bn::core::RefreshMode> modes;
    for (const FakeDisplay::Call& call : display.calls) {
      if (call.op == Op::kRefresh) {
        modes.push_back(call.mode);
      }
    }
    return modes;
  }

  bn::test::TraceFixture<64> trace;
  bn::test::FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};
  bn::test::FakeStorage storage;
  SteppingClock clock;
  FakeDisplay display;
  std::unique_ptr<DrawingViewer> viewer;

 private:
  std::size_t lastCalls_ = 0;
  int quietTurns_ = 0;
};

TEST(DrawingViewer, ShowsTheDrawingsInItsFolderInTheOrderOfTheirNames) {
  Fixture fixture;
  fixture.Put("b.svg", Svg(40, 24, "<rect width='40' height='24'/>"));
  fixture.Put("a.SVG", Svg(40, 24, ""));
  fixture.Put("notes.txt", "Not a drawing.");
  fixture.Put("inner/c.svg", Svg(40, 24, ""));
  // Hidden, as what a Mac writes beside each file it copies to a card is.
  fixture.Put("._b.svg", "Not a drawing.");
  fixture.Put(".d.png", "Not a picture.");
  fixture.Start();
  ASSERT_EQ(fixture.viewer->CurrentState(), State::kReady);
  EXPECT_EQ(fixture.viewer->Drawings(), 2U);
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  ASSERT_FALSE(fixture.Lines().empty());
  EXPECT_NE(fixture.Lines().back().find("drawing 1 of 2: a.SVG, ok"), std::string::npos) << fixture.Lines().back();
  // The first is shown by a full refresh, and the panel's voltages then
  // switched off.
  ASSERT_EQ(fixture.Refreshes(), std::vector<bn::core::RefreshMode>{bn::core::RefreshMode::kFull});
  EXPECT_EQ(fixture.BlackCount(), 0U);
  fixture.Press(bn::core::Button::kDown);
  fixture.Settle();
  EXPECT_EQ(fixture.BlackCount(), 40U * 24U);
  EXPECT_EQ(fixture.viewer->Current(), 1U);
}

TEST(DrawingViewer, WritesEachBandToBothPlanes) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<rect width='20' height='24'/>"));
  fixture.Start();
  fixture.Settle();
  // Two bands of 16 rows and 8, each to the new-image plane and the old.
  std::vector<std::pair<bn::core::DisplayPlane, std::uint16_t>> writes;
  for (const FakeDisplay::Call& call : fixture.display.calls) {
    if (call.op == Op::kWrite) {
      writes.emplace_back(call.plane, call.firstRow);
    }
  }
  const std::vector<std::pair<bn::core::DisplayPlane, std::uint16_t>> expected = {{bn::core::DisplayPlane::kNew, 0},
                                                                                  {bn::core::DisplayPlane::kOld, 0},
                                                                                  {bn::core::DisplayPlane::kNew, 16},
                                                                                  {bn::core::DisplayPlane::kOld, 16}};
  EXPECT_EQ(writes, expected);
  EXPECT_EQ(fixture.display.memory[0], fixture.display.memory[1]);
  for (std::size_t y = 0; y < 24; ++y) {
    EXPECT_TRUE(fixture.Black(0, y) && fixture.Black(19, y) && !fixture.Black(20, y) && !fixture.Black(39, y)) << y;
  }
}

TEST(DrawingViewer, FitsADrawingToThePageAsLargeAsItGoesInTheMiddle) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(10, 10, "<rect width='10' height='10'/>"));
  fixture.Start();
  fixture.Settle();
  // 24 by 24, with 8 on either side.
  for (std::size_t x = 0; x < 40; ++x) {
    EXPECT_EQ(fixture.Black(x, 12), x >= 8 && x < 32) << x;
  }
  EXPECT_EQ(fixture.BlackCount(), 24U * 24U);
}

TEST(DrawingViewer, DrawsAHairlineAPixelWideAndSolid) {
  // A tenth of a pixel as it is, which dithered would be a few dots.
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<line x1='2' y1='12.5' x2='38' y2='12.5' stroke='#000' stroke-width='.1'/>"));
  fixture.Start();
  fixture.Settle();
  for (std::size_t x = 0; x < 40; ++x) {
    EXPECT_EQ(fixture.Black(x, 12), x >= 2 && x < 38) << x;
  }
  EXPECT_EQ(fixture.BlackCount(), 36U);
}

TEST(DrawingViewer, TurnsTheDrawingTheWayThePanelIsTurnedToBeRead) {
  // The panel's picture is turned a quarter clockwise to be read: a page
  // 24 wide and 40 high, whose top is the panel's left.
  Fixture fixture;
  fixture.display.turn = bn::core::PanelTurn::kClockwise;
  fixture.Put("a.svg", Svg(24, 40, "<rect width='24' height='10'/><rect width='6' height='40'/>"));
  fixture.Start();
  fixture.Settle();
  for (std::size_t y = 0; y < 24; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      // The page's top ten rows are the panel's first ten columns, and its
      // left six columns the panel's bottom six rows.
      EXPECT_EQ(fixture.Black(x, y), x < 10 || y >= 18) << x << "," << y;
    }
  }
}

TEST(DrawingViewer, AGrayComesOutAsDotsAsDarkOnTheWhole) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<rect width='40' height='24' fill='#808080'/>"));
  fixture.Start();
  fixture.Settle();
  const std::size_t black = fixture.BlackCount();
  EXPECT_NEAR(static_cast<double>(black), (40.0 * 24.0) / 2, 20.0);
}

TEST(DrawingViewer, ATourShowsEachDrawingTwiceByItselfTheSecondTimeFromTheCacheAndStops) {
  Fixture fixture;
  for (const char* name : {"1.svg", "2.svg", "3.svg"}) {
    fixture.Put(name, Svg(40, 24, "<circle cx='20' cy='12' r='9'/>"));
  }
  fixture.Start(true);
  for (int settled = 0; settled < 12; ++settled) {
    fixture.Settle();
  }
  EXPECT_EQ(fixture.viewer->Shown(), 6U);
  EXPECT_EQ(fixture.viewer->Current(), 2U);
  const std::vector<std::string> lines = fixture.Lines();
  ASSERT_EQ(lines.size(), 7U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(lines[i].find("drawing " + std::to_string(i + 1) + " of 3: "), 0U) << lines[i];
    EXPECT_EQ(lines[i].find(", from the cache"), std::string::npos) << lines[i];
    EXPECT_EQ(lines[i + 3].find("drawing " + std::to_string(i + 1) + " of 3: " + std::to_string(i + 1) +
                                ".svg, from the cache, "),
              0U)
        << lines[i + 3];
  }
  EXPECT_EQ(lines[6], "drawings: the tour is over, 6 shown");
  // Each is timed, and the cache says what it did.
  EXPECT_EQ(fixture.Lines(true).size(), 9U);
  const std::vector<std::string> cached = fixture.CacheLines();
  ASSERT_EQ(cached.size(), 6U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(cached[i].find("drawing " + std::to_string(i + 1) + " cache: kept, "), 0U) << cached[i];
    EXPECT_EQ(cached[i + 3].find("drawing " + std::to_string(i + 1) + " cache: read "), 0U) << cached[i + 3];
  }
}

TEST(DrawingViewer, ATourOfOneDrawingShowsItTwice) {
  Fixture fixture;
  fixture.Put("1.svg", Svg(40, 24, "<circle cx='20' cy='12' r='9'/>"));
  fixture.Start(true);
  for (int settled = 0; settled < 4; ++settled) {
    fixture.Settle();
  }
  EXPECT_EQ(fixture.viewer->Shown(), 2U);
  const std::vector<std::string> lines = fixture.Lines();
  ASSERT_EQ(lines.size(), 3U);
  EXPECT_EQ(lines[1].find("drawing 1 of 1: 1.svg, from the cache, "), 0U) << lines[1];
  EXPECT_EQ(lines[2], "drawings: the tour is over, 2 shown");
}

TEST(DrawingViewer, ATourPassesOverWhatItCannotShowAndGoesOn) {
  // The first, one in the middle and the last cannot be shown: each line
  // says why, and the tour goes on to the end.
  Fixture fixture;
  const std::string text = "Not a picture.";
  for (const char* name : {"a.png", "c.png", "e.png"}) {
    fixture.storage.Put(std::string("/drawings/") + name, {text.begin(), text.end()});
  }
  fixture.Put("b.svg", Svg(40, 24, ""));
  fixture.Put("d.svg", Svg(40, 24, ""));
  fixture.Start(true);
  for (int settled = 0; settled < 16; ++settled) {
    fixture.Settle();
  }
  EXPECT_EQ(fixture.viewer->Shown(), 4U);
  EXPECT_EQ(fixture.viewer->Current(), 4U);
  const std::vector<std::string> lines = fixture.Lines();
  ASSERT_EQ(lines.size(), 11U);
  for (const std::size_t lap : {std::size_t{0}, std::size_t{5}}) {
    EXPECT_EQ(lines[lap + 0], "picture 1 of 5: a.png, not shown: not png");
    EXPECT_EQ(lines[lap + 1].find("drawing 2 of 5: b.svg, "), 0U) << lines[lap + 1];
    EXPECT_EQ(lines[lap + 2], "picture 3 of 5: c.png, not shown: not png");
    EXPECT_EQ(lines[lap + 3].find("drawing 4 of 5: d.svg, "), 0U) << lines[lap + 3];
    EXPECT_EQ(lines[lap + 4], "picture 5 of 5: e.png, not shown: not png");
  }
  EXPECT_EQ(lines[10], "drawings: the tour is over, 4 shown");
}

TEST(DrawingViewer, DownAndUpGoFromOneToTheNextAndRoundAndBackAndConfirmRedraw) {
  Fixture fixture;
  for (const char* name : {"1.svg", "2.svg", "3.svg"}) {
    fixture.Put(name, Svg(40, 24, ""));
  }
  fixture.Start();
  fixture.Settle();
  fixture.Press(bn::core::Button::kUp);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Current(), 2U);
  fixture.Press(bn::core::Button::kRight);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Current(), 0U);
  fixture.Press(bn::core::Button::kBack);
  fixture.Settle();
  fixture.Press(bn::core::Button::kConfirm);
  fixture.Settle();
  using bn::core::RefreshMode;
  EXPECT_EQ(fixture.Refreshes(), (std::vector<RefreshMode>{RefreshMode::kFull, RefreshMode::kHalf, RefreshMode::kHalf,
                                                           RefreshMode::kFull, RefreshMode::kHalf}));
  EXPECT_EQ(fixture.viewer->Shown(), 5U);
}

TEST(DrawingViewer, APressWhileADrawingIsBeingDrawnGivesItUpForTheOneAskedFor) {
  Fixture fixture;
  std::string many;
  for (int shape = 0; shape < 400; ++shape) {
    many += "<rect x='" + std::to_string(shape % 30) + "' width='3' height='3'/>";
  }
  fixture.Put("1.svg", Svg(40, 24, many));
  fixture.Put("2.svg", Svg(40, 24, "<rect width='40' height='24'/>"));
  fixture.clock.step = 1000;  // So that a turn draws little.
  fixture.Start();
  fixture.viewer->Tick();
  fixture.viewer->Tick();
  EXPECT_TRUE(fixture.Refreshes().empty());
  fixture.Press(bn::core::Button::kDown);
  fixture.clock.step = 10;
  fixture.Settle();
  EXPECT_EQ(fixture.Refreshes().size(), 1U);
  EXPECT_EQ(fixture.viewer->Current(), 1U);
  EXPECT_EQ(fixture.BlackCount(), 40U * 24U);
  EXPECT_EQ(fixture.storage.OpenCount(), 0);
}

TEST(DrawingViewer, ATurnDrawsForAboutItsSliceAndNoMore) {
  Fixture fixture;
  std::string many;
  for (int shape = 0; shape < 2000; ++shape) {
    many += "<path d='M" + std::to_string(shape % 40) + " 0l3 20-3 4z'/>";
  }
  fixture.Put("a.svg", Svg(40, 24, many));
  // Each reading of the clock a fifth of a millisecond on: enough that a
  // turn ends on its slice and not on the end of the drawing.
  fixture.clock.step = 200;
  fixture.Start();
  std::uint64_t longest = 0;
  int turns = 0;
  while (fixture.viewer->Shown() == 0 && turns < 100000) {
    fixture.display.busy = false;
    const std::uint64_t before = fixture.clock.now;
    fixture.viewer->Tick();
    longest = std::max(longest, fixture.clock.now - before);
    ++turns;
  }
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_GT(turns, 5) << "a drawing this long takes several turns";
  // The clock is read every sixteen calls of the renderer, each a few
  // readings of it here.
  EXPECT_LE(longest, DrawingViewer::kSliceUs + 2000);
}

TEST(DrawingViewer, SaysWhatEachDrawingCameToAndWhatWasLeftOut) {
  Fixture fixture;
  fixture.Put("words.svg",
              Svg(40, 24, "<rect width='4' height='4' stroke='#000'/><line x2='9' stroke='#000'/><text>Words</text>"));
  fixture.Start();
  fixture.Settle();
  ASSERT_FALSE(fixture.Lines().empty());
  const std::string line = fixture.Lines().back();
  EXPECT_NE(line.find("drawing 1 of 1: words.svg, ok, 1 shape, 2 strokes,"), std::string::npos) << line;
  EXPECT_NE(line.find("left out: text,"), std::string::npos) << line;
}

TEST(DrawingViewer, SaysWhereEachDrawingsTimeWentOnceItIsShown) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<rect width='40' height='24'/>"));
  fixture.Start();
  fixture.Settle();
  const std::vector<std::string> timed = fixture.Lines(true);
  ASSERT_EQ(timed.size(), 2U);
  // 24 rows in bands of 16 is two bands.
  EXPECT_EQ(timed[0].find("drawing 1 timed: 2 bands, 0 KB read in "), 0U) << timed[0];
  for (const std::string_view part : {" ms; rendering ", " ms, dithering ", " ms, writing ", " ms, waiting ",
                                      " ms, other ", " ms, between turns ", " ms, refreshing "}) {
    EXPECT_NE(timed[0].find(part), std::string::npos) << part << " in " << timed[0];
  }
  // What was kept for the two bands, a chunk of 512 bytes each, written
  // and read back.
  EXPECT_EQ(timed[1].find("drawing 1 timed: spool 1 KB written in "), 0U) << timed[1];
  EXPECT_NE(timed[1].find(" ms (2 writes), 1 KB read in "), std::string::npos) << timed[1];
  EXPECT_NE(timed[1].find(" ms; replaying "), std::string::npos) << timed[1];
  // A drawing given up for another is not timed.
  EXPECT_EQ(fixture.Lines().size(), 1U);
}

TEST(DrawingViewer, ReadsADrawingOnceForItsSizeAndOnceToKeepWhatItDraws) {
  // A file of several kilobytes, read a kilobyte at a time: through once to
  // its first tag, and once whole, however many bands the panel has.
  Fixture fixture;
  std::string body;
  for (int i = 0; i < 200; ++i) {
    body += "<circle cx='" + std::to_string(i % 40) + "' cy='" + std::to_string(i % 24) + "' r='3'/>";
  }
  const std::string file = Svg(40, 24, body);
  fixture.Put("a.svg", file);
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  // Each read of the drawing is a kilobyte; the spool's are 512 bytes, and
  // add up to what was kept.
  std::size_t drawingReads = 0;
  for (const auto& call : fixture.storage.reads) {
    drawingReads += call.size == DrawingViewer::kReadBytes ? 1 : 0;
  }
  EXPECT_EQ(drawingReads, 1 + ((file.size() + DrawingViewer::kReadBytes - 1) / DrawingViewer::kReadBytes));
}

TEST(DrawingViewer, WritesWhatItKeepsAFewKilobytesAtATime) {
  // A panel wide enough for a band of grays of several kilobytes, and a drawing
  // that keeps many chunks: they go to the card gathered, as many to a
  // write as the band of grays holds, 400 by 16 bytes: 12 chunks,
  // and the panel shows what it shows when nothing is kept.
  std::string body;
  for (int i = 0; i < 300; ++i) {
    body += "<circle cx='" + std::to_string((i * 37) % 400) + "' cy='" + std::to_string((i * 23) % 240) + "' r='" +
            std::to_string(3 + (i % 9)) + "' fill='#" + std::to_string(100 + (i % 800)) + "'/>";
  }
  const std::string file = Svg(400, 240, body);
  Fixture kept;
  kept.display.Resize({.width = 400, .height = 240});
  kept.Put("a.svg", file);
  kept.Start();
  kept.Settle();
  Fixture direct;
  direct.display.Resize({.width = 400, .height = 240});
  direct.Put("a.svg", file);
  direct.storage.failWrites = bn::core::StorageStatus::kFailed;
  direct.Start();
  direct.Settle();
  ASSERT_EQ(kept.viewer->Shown(), 1U);
  ASSERT_EQ(direct.viewer->Shown(), 1U);
  for (std::size_t y = 0; y < 240; ++y) {
    for (std::size_t x = 0; x < 400; ++x) {
      ASSERT_EQ(kept.Black(x, y), direct.Black(x, y)) << x << "," << y;
    }
  }
  const std::size_t chunks =
      kept.storage.entries.at(std::string(DrawingViewer::kSpoolPath)).data.size() / bn::draw::SpoolWriter::kChunkBytes;
  ASSERT_GT(chunks, 32U);
  // The spool's line says how many writes it took.
  const std::vector<std::string> timed = kept.Lines(true);
  ASSERT_EQ(timed.size(), 2U);
  const std::size_t at = timed[1].find(" ms (");
  ASSERT_NE(at, std::string::npos) << timed[1];
  const std::size_t writes = std::strtoul(timed[1].c_str() + at + 5, nullptr, 10);
  EXPECT_GT(writes, 0U);
  EXPECT_LE(writes, (chunks / 12) + 1) << timed[1];
}

TEST(DrawingViewer, DrawsAsBeforeWhenTheCardWillNotKeepTheDrawing) {
  const std::string file = Svg(40, 24, "<circle cx='20' cy='12' r='9'/><rect x='2' y='2' width='9' height='20'/>");
  Fixture kept;
  kept.Put("a.svg", file);
  kept.Start();
  kept.Settle();
  Fixture refused;
  refused.Put("a.svg", file);
  refused.storage.failWrites = bn::core::StorageStatus::kFailed;
  refused.Start();
  refused.Settle();
  ASSERT_EQ(refused.viewer->Shown(), 1U);
  EXPECT_EQ(refused.BlackCount(), kept.BlackCount());
  EXPECT_GT(refused.BlackCount(), 100U);
  const std::vector<std::string> timed = refused.Lines(true);
  ASSERT_EQ(timed.size(), 2U);
  EXPECT_EQ(timed[1], "drawing 1 timed: not spooled: store failed");
}

TEST(DrawingViewer, WithNoDrawingsLeavesThePanelBlankAndSaysWhy) {
  Fixture fixture;
  fixture.Start();
  fixture.Settle(100);
  EXPECT_EQ(fixture.viewer->Drawings(), 0U);
  EXPECT_TRUE(fixture.Refreshes().empty());
  ASSERT_FALSE(fixture.Lines().empty());
  EXPECT_EQ(fixture.Lines().back(), "drawings: none in /drawings");
  // A file that is not a drawing, or a broken one, is drawn as far as it
  // goes and said to be what it is.
  Fixture broken;
  broken.Put("a.svg", "<html/>");
  broken.Put("b.svg", Svg(40, 24, "<rect width='40' height='24'/><<"));
  broken.Start();
  broken.Settle();
  broken.Press(bn::core::Button::kDown);
  broken.Settle();
  const std::vector<std::string> lines = broken.Lines();
  ASSERT_GE(lines.size(), 2U);
  EXPECT_NE(lines[lines.size() - 2].find("a.svg, not svg"), std::string::npos) << lines[lines.size() - 2];
  EXPECT_NE(lines.back().find("b.svg, malformed"), std::string::npos) << lines.back();
  EXPECT_EQ(broken.BlackCount(), 40U * 24U);
}

TEST(DrawingViewer, SleepsWhenAskedOnceThePanelIsAtRest) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<rect width='4' height='4'/>"));
  fixture.Start();
  fixture.Settle();
  fixture.viewer->Sleep();
  EXPECT_FALSE(fixture.viewer->Resting());
  fixture.viewer->Tick();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kAsleep);
  EXPECT_TRUE(fixture.viewer->Resting());
  EXPECT_EQ(fixture.display.calls.back().op, Op::kSleep);
}

TEST(DrawingViewer, FailsWithNoPanel) {
  Fixture fixture;
  fixture.display.present = false;
  fixture.Put("a.svg", Svg(40, 24, ""));
  fixture.Start();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kFailed);
  EXPECT_TRUE(fixture.viewer->Resting());
}

// ---- Pictures ----

// A PNG of gray, `width` by `height`, each pixel what `gray` says of it.
template <typename Gray>
std::vector<std::uint8_t> GrayPng(std::uint32_t width, std::uint32_t height, Gray gray, std::uint8_t interlace = 0) {
  bn::test::PngPicture picture;
  picture.width = width;
  picture.height = height;
  picture.depth = 8;
  picture.colour = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      picture.rows.push_back(static_cast<std::uint8_t>(gray(x, y)));
    }
  }
  const std::array<std::uint8_t, 1> none = {0};
  std::vector<std::uint8_t> file = bn::test::PngFor(picture, none);
  if (interlace != 0) {
    // The header's last byte, and its checksum made right again.
    bn::test::PngFile again;
    again.Header(width, height, 8, 0, interlace);
    std::copy(again.Bytes().begin() + 8, again.Bytes().end(), file.begin() + 8);
  }
  return file;
}

TEST(DrawingViewer, ShowsAPictureFittedToThePageInTheMiddle) {
  // Black, twice as wide as high: the page's width, and half its height
  // and a bit, in the middle down it.
  Fixture fixture;
  fixture.storage.Put("/drawings/p.png", GrayPng(20, 10, [](auto, auto) { return 0; }));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U) << (fixture.Lines().empty() ? "" : fixture.Lines().back());
  for (std::size_t y = 0; y < 24; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      EXPECT_EQ(fixture.Black(x, y), y >= 2 && y < 22) << x << "," << y;
    }
  }
  ASSERT_EQ(fixture.Refreshes(), std::vector<bn::core::RefreshMode>{bn::core::RefreshMode::kFull});
}

TEST(DrawingViewer, TurnsAPictureTheWayThePanelIsTurnedToBeRead) {
  // Turned a quarter clockwise to be read, the page's top is the panel's
  // left: a picture black above and white below is black on the left.
  Fixture fixture;
  fixture.display.turn = bn::core::PanelTurn::kClockwise;
  fixture.storage.Put("/drawings/p.png", GrayPng(24, 40, [](auto, std::uint32_t y) { return y < 20 ? 0 : 255; }));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  for (std::size_t y = 0; y < 24; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      EXPECT_EQ(fixture.Black(x, y), x < 20) << x << "," << y;
    }
  }
}

TEST(DrawingViewer, SetsAPictureIntoTheWholePanelAndWritesItOnce) {
  // A panel of 400 rows, black down to the middle and white below: set from
  // one reading of the picture, and written whole to both planes.
  Fixture fixture;
  fixture.display.Resize({.width = 40, .height = 400});
  fixture.storage.Put("/drawings/p.png", GrayPng(4, 40, [](auto, std::uint32_t y) { return y < 20 ? 0 : 255; }));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  for (std::size_t y = 0; y < 400; ++y) {
    // Away from the edge, where black and white are blended and dithered.
    if (y < 180 || y >= 220) {
      for (std::size_t x = 0; x < 40; ++x) {
        ASSERT_EQ(fixture.Black(x, y), y < 200) << x << "," << y;
      }
    }
  }
  std::vector<FakeDisplay::Call> writes;
  for (const FakeDisplay::Call& call : fixture.display.calls) {
    if (call.op == Op::kWrite) {
      writes.push_back(call);
    }
  }
  ASSERT_EQ(writes.size(), 2U);
  EXPECT_EQ(writes[0].plane, bn::core::DisplayPlane::kNew);
  EXPECT_EQ(writes[1].plane, bn::core::DisplayPlane::kOld);
  for (const FakeDisplay::Call& write : writes) {
    EXPECT_EQ(write.firstRow, 0U);
    EXPECT_EQ(write.rowCount, 400U);
  }
}

TEST(DrawingViewer, ReadsAPictureThroughOnce) {
  // Its header, and then the whole file once, on a panel that the picture
  // would once have been read three times for.
  Fixture fixture;
  fixture.display.Resize({.width = 40, .height = 400});
  const std::vector<std::uint8_t> file = GrayPng(300, 300, [](std::uint32_t x, std::uint32_t y) {
    // Grays that do not compress.
    return static_cast<std::uint8_t>(((x * 2654435761U) ^ (y * 2246822519U)) >> 24U);
  });
  ASSERT_GT(file.size(), 32768U);
  fixture.storage.Put("/drawings/p.png", file);
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  std::size_t asked = 0;
  for (const auto& read : fixture.storage.reads) {
    asked += read.size;
  }
  EXPECT_LT(asked, file.size() + 16384) << "of " << file.size();
}

TEST(DrawingViewer, AGrayPictureComesOutAsDotsAsDarkOnTheWhole) {
  Fixture fixture;
  fixture.storage.Put("/drawings/p.png", GrayPng(40, 24, [](auto, auto) { return 128; }));
  fixture.Start();
  fixture.Settle();
  const std::size_t black = fixture.BlackCount();
  EXPECT_GT(black, 40U * 24U * 2 / 5);
  EXPECT_LT(black, 40U * 24U * 3 / 5);
}

TEST(DrawingViewer, SaysWhatEachPictureCameToAndWhatItTook) {
  Fixture fixture;
  fixture.storage.Put("/drawings/p.png", GrayPng(20, 10, [](auto, auto) { return 0; }));
  fixture.Start();
  fixture.Settle();
  ASSERT_FALSE(fixture.Lines().empty());
  const std::string line = fixture.Lines().back();
  EXPECT_EQ(line.rfind("picture 1 of 1: p.png, ok, 20x10 gray shown 40x20: reading ", 0), 0U) << line;
  for (const char* part : {" ms, scaling ", " ms, dithering ", " ms, writing "}) {
    EXPECT_NE(line.find(part), std::string::npos) << part << " in " << line;
  }
  EXPECT_EQ(line.substr(line.size() - 3), " ms") << line;
}

TEST(DrawingViewer, ShowsPicturesAndDrawingsTogetherInTheOrderOfTheirNames) {
  Fixture fixture;
  fixture.storage.Put("/drawings/a.png", GrayPng(40, 24, [](auto, auto) { return 0; }));
  fixture.Put("b.svg", Svg(40, 24, ""));
  fixture.Start();
  EXPECT_EQ(fixture.viewer->Drawings(), 2U);
  fixture.Settle();
  EXPECT_EQ(fixture.BlackCount(), 40U * 24U);
  ASSERT_FALSE(fixture.Lines().empty());
  EXPECT_EQ(fixture.Lines().back().rfind("picture 1 of 2: a.png, ok", 0), 0U) << fixture.Lines().back();
  fixture.Press(bn::core::Button::kDown);
  fixture.Settle();
  EXPECT_EQ(fixture.BlackCount(), 0U);
  EXPECT_EQ(fixture.Lines().back().rfind("drawing 2 of 2: b.svg, ok", 0), 0U) << fixture.Lines().back();
  fixture.Press(bn::core::Button::kDown);
  fixture.Settle();
  EXPECT_EQ(fixture.BlackCount(), 40U * 24U);
}

TEST(DrawingViewer, SaysWhyAPictureIsNotShownAndLeavesThePanelAlone) {
  const auto notShown = [](const std::vector<std::uint8_t>& file) {
    Fixture fixture;
    fixture.storage.Put("/drawings/p.png", file);
    fixture.Start();
    fixture.Settle();
    EXPECT_EQ(fixture.viewer->Shown(), 0U);
    EXPECT_TRUE(fixture.Refreshes().empty());
    return fixture.Lines().empty() ? std::string() : fixture.Lines().back();
  };
  const std::string text = "Not a picture.";
  EXPECT_EQ(notShown({text.begin(), text.end()}), "picture 1 of 1: p.png, not shown: not png");
  EXPECT_EQ(notShown(GrayPng(4, 4, [](auto, auto) { return 0; }, 1)), "picture 1 of 1: p.png, not shown: interlaced");
  // Wider than the memory for its rows allows.
  EXPECT_EQ(notShown(GrayPng(9000, 1, [](auto, auto) { return 0; })),
            "picture 1 of 1: p.png, not shown: its rows need 18002 bytes");
}

// A gray JPEG, `width` by `height`, each block of 8 by 8 black or white as
// `black` says of it; progressive if asked, and CMYK if asked.
template <typename Black>
std::vector<std::uint8_t> GrayJpeg(std::uint32_t width, std::uint32_t height, Black black, bool progressive = false,
                                   bool cmyk = false) {
  bn::test::JpegPicture picture;
  picture.width = width;
  picture.height = height;
  if (cmyk) {
    picture.components = {{1, 1, 1, 0}, {2, 1, 1, 0}, {3, 1, 1, 0}, {4, 1, 1, 0}};
  }
  picture.Make(8);
  for (std::size_t i = 0; i < picture.components.size(); ++i) {
    for (std::uint32_t y = 0; y < picture.BlocksDown(i); ++y) {
      for (std::uint32_t x = 0; x < picture.BlocksAcross(i); ++x) {
        // An average of -128 or 127 steps of 8: black, or white.
        picture.Block(i, x, y)[0] = static_cast<std::int16_t>(black(x, y) ? -128 : 127);
      }
    }
  }
  bn::test::JpegOptions options;
  options.frame = progressive ? 0xC2 : 0xC0;
  return bn::test::JpegFile(picture, options);
}

TEST(DrawingViewer, ShowsAJpegReadAsSmallAsItCanBeAndStillBeAsLargeAsItIsShown) {
  // 160 by 96, black on the left half and white on the right, shown 40 by
  // 24: read at a quarter, which is that size.
  Fixture fixture;
  fixture.storage.Put("/drawings/p.jpg", GrayJpeg(160, 96, [](std::uint32_t x, auto) { return x < 10; }));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U) << (fixture.Lines().empty() ? "" : fixture.Lines().back());
  for (std::size_t y = 0; y < 24; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      EXPECT_EQ(fixture.Black(x, y), x < 20) << x << "," << y;
    }
  }
  const std::string line = fixture.Lines().back();
  EXPECT_EQ(line.rfind("picture 1 of 1: p.jpg, ok, 160x96 gray read at 1/4 shown 40x24: reading ", 0), 0U) << line;
}

TEST(DrawingViewer, ShowsAProgressiveJpegFromItsFirstScanAtAnEighth) {
  Fixture fixture;
  fixture.storage.Put("/drawings/p.JPEG",
                      GrayJpeg(160, 96, [](std::uint32_t x, auto) { return x < 10; }, /*progressive=*/true));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U) << (fixture.Lines().empty() ? "" : fixture.Lines().back());
  // Read 20 by 12 and made twice the size: black on the left half still,
  // away from the middle, where black and white are blended.
  for (std::size_t y = 0; y < 24; ++y) {
    for (std::size_t x = 0; x < 40; ++x) {
      if (x < 18 || x >= 22) {
        EXPECT_EQ(fixture.Black(x, y), x < 20) << x << "," << y;
      }
    }
  }
  const std::string line = fixture.Lines().back();
  EXPECT_EQ(line.rfind("picture 1 of 1: p.JPEG, ok, 160x96 progressive gray read at 1/8 shown 40x24: reading ", 0), 0U)
      << line;
}

TEST(DrawingViewer, SaysWhyAJpegIsNotShown) {
  Fixture fixture;
  fixture.storage.Put("/drawings/p.jpg", GrayJpeg(16, 16, [](auto, auto) { return true; }, false, /*cmyk=*/true));
  fixture.Start();
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Shown(), 0U);
  ASSERT_FALSE(fixture.Lines().empty());
  EXPECT_EQ(fixture.Lines().back(), "picture 1 of 1: p.jpg, not shown: unsupported");
}

TEST(DrawingViewer, APressWhileAPictureIsBeingReadGivesItUpForTheOneAskedFor) {
  Fixture fixture;
  fixture.clock.step = 1000;  // So that a turn reads little.
  fixture.storage.Put("/drawings/a.png", GrayPng(400, 400, [](std::uint32_t x, std::uint32_t y) {
                        return static_cast<std::uint8_t>((x * 7) ^ (y * 13));
                      }));
  fixture.Put("b.svg", Svg(40, 24, "<rect width='40' height='24'/>"));
  fixture.Start();
  for (int turn = 0; turn < 3; ++turn) {
    fixture.viewer->Tick();
  }
  ASSERT_EQ(fixture.viewer->Shown(), 0U);
  fixture.Press(bn::core::Button::kDown);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Current(), 1U);
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_EQ(fixture.BlackCount(), 40U * 24U);
}

TEST(DrawingViewer, ATurnReadsAPictureForAboutItsSliceAndNoMore) {
  Fixture fixture;
  fixture.clock.step = 500;
  fixture.storage.Put("/drawings/a.png", GrayPng(600, 600, [](std::uint32_t x, std::uint32_t y) {
                        return static_cast<std::uint8_t>((x * 3) + (y * 5));
                      }));
  fixture.Start();
  int turns = 0;
  std::uint64_t longest = 0;
  while (fixture.viewer->Shown() == 0 && turns < 100000) {
    fixture.display.busy = false;
    const std::uint64_t before = fixture.clock.now;
    fixture.viewer->Tick();
    longest = std::max(longest, fixture.clock.now - before);
    ++turns;
  }
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_GT(turns, 5) << "a picture this large takes several turns";
  // A turn goes on past its slice by one step of the reader at the most:
  // a few hundred readings of the clock here.
  EXPECT_LT(longest, DrawingViewer::kSliceUs + 400'000) << longest;
}

// ---- The cache ----

// What the panel shows.
std::vector<std::uint8_t> Shown(const Fixture& fixture) { return fixture.display.shown; }

// The files of the cache.
std::vector<std::string> CacheFiles(const Fixture& fixture) {
  std::vector<std::string> files;
  for (const auto& [path, entry] : fixture.storage.entries) {
    if (path.starts_with(std::string(DrawingViewer::kCacheFolder) + "/")) {
      files.push_back(path);
    }
  }
  return files;
}

TEST(DrawingViewer, ShowsWhatItHasShownBeforeFromTheCacheJustAsItWasDrawn) {
  // A drawing of grays with shapes reaching past its edges, and a picture
  // of grays, neither the page's shape, the panel held each way: the second
  // time, from the cache, the panel shows what it did the first.
  const std::string drawing = Svg(37, 19,
                                  "<circle cx='0' cy='0' r='15' fill='#777'/><rect x='20' y='-5' width='30' "
                                  "height='10' fill='#333'/><ellipse cx='18' cy='12' rx='9' ry='4' fill='#bbb'/>");
  const std::vector<std::uint8_t> picture =
      GrayPng(17, 9, [](std::uint32_t x, std::uint32_t y) { return ((x * 15) + (y * 20)) % 256; });
  for (const bn::core::PanelTurn turn : {bn::core::PanelTurn::kNone, bn::core::PanelTurn::kClockwise,
                                         bn::core::PanelTurn::kHalf, bn::core::PanelTurn::kAnticlockwise}) {
    for (const bool isPicture : {false, true}) {
      Fixture fixture;
      fixture.display.turn = turn;
      if (isPicture) {
        fixture.storage.Put("/drawings/p.png", picture);
      } else {
        fixture.Put("a.svg", drawing);
      }
      fixture.Start();
      fixture.Settle();
      ASSERT_EQ(fixture.viewer->Shown(), 1U);
      const std::vector<std::uint8_t> drawn = Shown(fixture);
      ASSERT_EQ(CacheFiles(fixture).size(), 1U);
      fixture.Press(bn::core::Button::kConfirm);
      fixture.Settle();
      ASSERT_EQ(fixture.viewer->Shown(), 2U);
      const std::vector<std::string> lines = fixture.Lines();
      ASSERT_EQ(lines.size(), 2U);
      EXPECT_NE(lines[1].find(", from the cache, "), std::string::npos) << lines[1];
      EXPECT_EQ(Shown(fixture), drawn) << static_cast<int>(turn) << " " << isPicture;
      const std::vector<std::string> cached = fixture.CacheLines();
      ASSERT_EQ(cached.size(), 2U);
      EXPECT_NE(cached[0].find(" cache: kept, "), std::string::npos) << cached[0];
      EXPECT_NE(cached[1].find(" cache: read "), std::string::npos) << cached[1];
      EXPECT_EQ(fixture.storage.OpenCount(), 0);
    }
  }
}

TEST(DrawingViewer, DrawsAgainWhatHasChangedSinceItWasKept) {
  // The same size, other shapes: what was kept is not shown.
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<rect x='0' width='20' height='24'/>"));
  fixture.Start();
  fixture.Settle();
  EXPECT_TRUE(fixture.Black(5, 12));
  fixture.Put("a.svg", Svg(40, 24, "<rect x='20' width='20' height='24'/>"));
  fixture.Press(bn::core::Button::kConfirm);
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 2U);
  EXPECT_FALSE(fixture.Black(5, 12));
  EXPECT_TRUE(fixture.Black(35, 12));
  const std::vector<std::string> lines = fixture.Lines();
  EXPECT_EQ(lines[1].find(", from the cache"), std::string::npos) << lines[1];
  // And it is kept as it is now.
  fixture.Press(bn::core::Button::kConfirm);
  fixture.Settle();
  EXPECT_NE(fixture.Lines()[2].find(", from the cache, "), std::string::npos) << fixture.Lines()[2];
  EXPECT_TRUE(fixture.Black(35, 12));
}

TEST(DrawingViewer, DrawsAgainAndKeepsAgainWhatItFindsDamaged) {
  for (const bool cut : {false, true}) {
    Fixture fixture;
    fixture.Put("a.svg", Svg(40, 24, "<circle cx='20' cy='12' r='9' fill='#555'/>"));
    fixture.Start();
    fixture.Settle();
    const std::vector<std::uint8_t> drawn = Shown(fixture);
    // A byte of its last row changed, or the file cut short.
    for (const std::string& path : CacheFiles(fixture)) {
      bn::test::FakeStorage::Bytes& data = fixture.storage.entries.at(path).data;
      if (cut) {
        data.resize(data.size() - 3);
      } else {
        data.back() ^= 0x01U;
      }
    }
    fixture.Press(bn::core::Button::kConfirm);
    fixture.Settle();
    ASSERT_EQ(fixture.viewer->Shown(), 2U);
    EXPECT_EQ(Shown(fixture), drawn);
    EXPECT_EQ(fixture.Lines()[1].find(", from the cache"), std::string::npos) << fixture.Lines()[1];
    const std::vector<std::string> cached = fixture.CacheLines();
    ASSERT_EQ(cached.size(), 2U);
    EXPECT_NE(cached[1].find(" cache: kept, "), std::string::npos) << cached[1];
    EXPECT_NE(cached[1].find(", after it was found damaged"), std::string::npos) << cached[1];
    fixture.Press(bn::core::Button::kConfirm);
    fixture.Settle();
    EXPECT_NE(fixture.Lines()[2].find(", from the cache, "), std::string::npos) << fixture.Lines()[2];
    EXPECT_EQ(Shown(fixture), drawn);
  }
}

TEST(DrawingViewer, SaysWhenTheCardWillNotKeepWhatItShows) {
  Fixture fixture;
  fixture.Put("a.svg", Svg(40, 24, "<circle cx='20' cy='12' r='9'/>"));
  fixture.storage.failWrites = bn::core::StorageStatus::kNoSpace;
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_GT(fixture.BlackCount(), 0U);
  const std::vector<std::string> cached = fixture.CacheLines();
  ASSERT_EQ(cached.size(), 1U);
  EXPECT_EQ(cached[0], "drawing 1 cache: not kept: store failed");
}

TEST(DrawingViewer, WhatWasGivenUpBeforeItWasAllKeptIsDrawnAgain) {
  // Two drawings on a panel of ten bands: the first given up for the
  // second once one of its bands is written, and gone back to. It is drawn
  // whole, not taken half kept from the cache.
  std::string body;
  for (int i = 0; i < 100; ++i) {
    body += "<circle cx='" + std::to_string((i * 7) % 40) + "' cy='" + std::to_string((i * 13) % 160) + "' r='" +
            std::to_string(2 + (i % 5)) + "' fill='#" + std::to_string(100 + (i % 800)) + "'/>";
  }
  const std::string file = Svg(40, 160, body);
  Fixture whole;
  whole.display.Resize({.width = 40, .height = 160});
  whole.Put("a.svg", file);
  whole.Start();
  whole.Settle();
  const std::vector<std::uint8_t> drawn = Shown(whole);

  Fixture fixture;
  fixture.display.Resize({.width = 40, .height = 160});
  fixture.Put("a.svg", file);
  fixture.Put("b.svg", Svg(40, 160, "<rect width='40' height='160'/>"));
  fixture.Start();
  const auto written = [&fixture] {
    return std::count_if(fixture.display.calls.begin(), fixture.display.calls.end(),
                         [](const FakeDisplay::Call& call) { return call.op == Op::kWrite; });
  };
  for (int turn = 0; turn < 100000 && written() < 2; ++turn) {
    fixture.display.busy = false;
    fixture.viewer->Tick();
  }
  ASSERT_EQ(written(), 2);
  fixture.Press(bn::core::Button::kDown);
  fixture.Settle();
  fixture.Press(bn::core::Button::kUp);
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 2U);
  const std::vector<std::string> lines = fixture.Lines();
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[1].find("drawing 1 of 2: a.svg, ok"), 0U) << lines[1];
  EXPECT_EQ(Shown(fixture), drawn);
  // What it found of the first time was not finished.
  const std::vector<std::string> cached = fixture.CacheLines();
  ASSERT_FALSE(cached.empty());
  EXPECT_NE(cached.back().find(", after it was found damaged"), std::string::npos) << cached.back();
}

}  // namespace
