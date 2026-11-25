#include "reader/app.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/endian.h"
#include "crash_fakes.h"
#include "display_fakes.h"
#include "font_image.h"
#include "input_fakes.h"
#include "memory_fakes.h"
#include "power_fakes.h"
#include "reader/book_check.h"
#include "reader/drawing_viewer.h"
#include "reader/layout_check.h"
#include "reader/text_viewer.h"
#include "reader/trace_ids.h"
#include "storage_fakes.h"
#include "stream_fakes.h"
#include "trace_fakes.h"
#include "zip_builder.h"

namespace {

using bn::core::FrameType;
using bn::test::StreamItem;

struct AppFixture {
  bn::test::FakeClock clock;
  bn::test::FakeSync sync;
  bn::test::FakeSink sink;
  bn::test::FakeMemoryProbe memory;
  bn::test::FakeCrashStore crash;
  bn::test::FakeDisplay display;  // Absent unless a test says otherwise.
  bn::test::FakeInput input;
  bn::test::FakeStorage storage;  // A card with nothing on it.
  bn::core::MemoryByteMap fonts;  // No fonts image unless a test gives one.
  bn::test::FakePower power;      // Cannot sleep unless a test says otherwise.
  bn::reader::App app{bn::reader::Services{.clock = clock,
                                           .traceSync = sync,
                                           .serial = sink,
                                           .memory = memory,
                                           .crash = crash,
                                           .display = display,
                                           .input = input,
                                           .storage = storage,
                                           .fonts = fonts,
                                           .power = power,
                                           .startCause = bn::core::StartCause::kWatchdog}};

  // The bytes of every crash frame sent so far, joined, after checking that
  // each piece continues the one before.
  std::vector<std::uint8_t> CrashBytes() const {
    std::vector<std::uint8_t> joined;
    for (const StreamItem& item : ItemsOfType(FrameType::kCrash)) {
      const std::span<const std::uint8_t> payload(item.payload);
      EXPECT_GT(payload.size(), 4u);
      if (payload.size() <= 4) {
        continue;
      }
      EXPECT_EQ(bn::core::LoadLe16(payload.subspan(0).first<2>()), joined.size());
      joined.insert(joined.end(), payload.begin() + 4, payload.end());
    }
    return joined;
  }

  // Everything sent in metrics frames so far, joined into one line.
  std::string Metrics() const {
    std::string all;
    for (const StreamItem& item : ItemsOfType(FrameType::kMetrics)) {
      all += (all.empty() ? "" : " ") + item.Text();
    }
    return all;
  }

  std::vector<StreamItem> Items() const { return bn::test::ParseStream(sink.received); }

  std::vector<StreamItem> ItemsOfType(FrameType type) const {
    std::vector<StreamItem> matching;
    for (StreamItem& item : Items()) {
      if (item.isFrame && item.type == type) {
        matching.push_back(std::move(item));
      }
    }
    return matching;
  }

  // The (id, arg) of every trace record sent so far.
  std::vector<std::pair<std::uint16_t, std::uint32_t>> TraceRecords() const {
    std::vector<std::pair<std::uint16_t, std::uint32_t>> records;
    for (const StreamItem& item : ItemsOfType(FrameType::kTrace)) {
      const std::span<const std::uint8_t> payload(item.payload);
      for (std::size_t at = 4; at + bn::core::kWireRecordSize <= payload.size(); at += bn::core::kWireRecordSize) {
        records.emplace_back(bn::core::LoadLe16(payload.subspan(at + 12).first<2>()),
                             bn::core::LoadLe32(payload.subspan(at + 8).first<4>()));
      }
    }
    return records;
  }

  // Advances the clock in small steps, ticking each time, like a main loop.
  void RunFor(std::uint64_t microseconds, std::uint64_t step = 50'000) {
    for (std::uint64_t elapsed = 0; elapsed < microseconds; elapsed += step) {
      clock.now += step;
      app.Tick();
    }
  }
};

constexpr auto kStartId = static_cast<std::uint16_t>(bn::reader::kSpanAppStart);
constexpr auto kHeartbeatId = static_cast<std::uint16_t>(bn::reader::kInstantHeartbeat);

TEST(App, DoesNothingBeforeStart) {
  AppFixture fixture;
  fixture.clock.now = 500;
  EXPECT_FALSE(fixture.app.Started());
  EXPECT_EQ(fixture.app.UptimeUs(), 0u);

  fixture.RunFor(10'000'000);
  EXPECT_TRUE(fixture.sink.received.empty());
  EXPECT_EQ(bn::core::ActiveTracer(), nullptr);
}

TEST(App, UptimeCountsFromStart) {
  AppFixture fixture;
  fixture.clock.now = 1'000;
  fixture.app.Start();
  EXPECT_TRUE(fixture.app.Started());
  EXPECT_EQ(fixture.app.UptimeUs(), 0u);

  fixture.clock.now = 3'500;
  EXPECT_EQ(fixture.app.UptimeUs(), 2'500u);
}

TEST(App, StartAnnouncesTheStreamWithItsVersion) {
  AppFixture fixture;
  fixture.app.Start();

  const auto hello = fixture.ItemsOfType(FrameType::kHello);
  ASSERT_EQ(hello.size(), 1u);
  ASSERT_FALSE(hello[0].payload.empty());
  EXPECT_EQ(hello[0].payload[0], bn::core::kStreamProtocolVersion);
  EXPECT_EQ(hello[0].Text(), bn::reader::Version());
  // And with why the device started, as its hardware has it.
  ASSERT_GE(hello[0].payload.size(), 2u);
  EXPECT_EQ(hello[0].payload[1], static_cast<std::uint8_t>(bn::core::StartCause::kWatchdog));
}

TEST(App, SendsNoCrashFramesWhenNothingCrashed) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.RunFor(2'000'000);
  EXPECT_TRUE(fixture.ItemsOfType(FrameType::kCrash).empty());
}

TEST(App, NamesItsVersionForAnyCrashReportItLeaves) {
  AppFixture fixture;
  EXPECT_EQ(fixture.crash.version, nullptr);
  fixture.app.Start();
  EXPECT_STREQ(fixture.crash.version, bn::reader::Version());
}

TEST(App, SendsASavedCrashReportAfterTheHello) {
  AppFixture fixture;
  fixture.crash.report = bn::test::SampleCrashReport();
  fixture.crash.hasReport = true;
  fixture.app.Start();

  // The first pieces go out with the start, right behind the hello.
  const std::vector<StreamItem> atStart = fixture.Items();
  ASSERT_GE(atStart.size(), 2u);
  EXPECT_EQ(atStart[0].type, FrameType::kHello);
  EXPECT_EQ(atStart[1].type, FrameType::kCrash);
  // A full report is too much for one go; the ticks that follow finish it.
  EXPECT_LT(fixture.CrashBytes().size(), bn::core::EncodedCrashReportSize(fixture.crash.report));

  fixture.RunFor(500'000);
  bn::core::CrashReport decoded{};
  ASSERT_TRUE(bn::core::DecodeCrashReport(fixture.CrashBytes(), decoded));
  EXPECT_EQ(bn::core::CrashText(decoded.reason), "Load access fault");
  EXPECT_EQ(decoded.bootsSince, 3u);

  // It is sent once per run.
  const std::size_t frames = fixture.ItemsOfType(FrameType::kCrash).size();
  fixture.RunFor(2'000'000);
  EXPECT_EQ(fixture.ItemsOfType(FrameType::kCrash).size(), frames);
}

TEST(App, KeepsTryingToSendACrashReportWhileTheSerialLineIsFull) {
  AppFixture fixture;
  fixture.crash.report = bn::test::SampleCrashReport();
  fixture.crash.hasReport = true;
  fixture.sink.full = true;
  fixture.app.Start();
  fixture.RunFor(1'000'000);
  EXPECT_TRUE(fixture.sink.received.empty());

  fixture.sink.full = false;
  fixture.RunFor(1'000'000);
  bn::core::CrashReport decoded{};
  ASSERT_TRUE(bn::core::DecodeCrashReport(fixture.CrashBytes(), decoded));
  EXPECT_EQ(decoded.stack, fixture.crash.report.stack);
}

TEST(App, FlushSendsTheRestOfACrashReport) {
  AppFixture fixture;
  fixture.crash.report = bn::test::SampleCrashReport();
  fixture.crash.hasReport = true;
  fixture.app.Start();
  fixture.app.Flush();

  bn::core::CrashReport decoded{};
  EXPECT_TRUE(bn::core::DecodeCrashReport(fixture.CrashBytes(), decoded));
}

TEST(App, ReportsAPanelThatCannotBeSetUpAndCarriesOn) {
  AppFixture fixture;  // Its panel is absent.
  fixture.app.Start();
  EXPECT_EQ(fixture.app.Viewer().CurrentState(), bn::reader::PatternViewer::State::kFailed);
  fixture.input.Press(bn::core::Button::kDown, fixture.clock.now);
  fixture.RunFor(5'100'000);
  EXPECT_NE(fixture.Metrics().find("panel_ok=0["), std::string::npos) << fixture.Metrics();
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kWrite), 0u);
}

TEST(App, PausesBeforeATurnOnlyWhenNoWorkIsWaiting) {
  // With no screen but the test pattern there is never work waiting.
  AppFixture plain;
  plain.display.present = true;
  plain.app.Start();
  EXPECT_EQ(plain.app.PauseUs(), bn::reader::App::kTurnPauseUs);

  // The drawing viewer, with drawings to draw: no pause while it draws,
  // but for one at least every kLongestRunUs; and a pause each turn once
  // they are drawn and shown.
  AppFixture fixture;
  fixture.display.present = true;
  std::string body;
  for (int i = 0; i < 400; ++i) {
    body += "<circle cx='" + std::to_string(i % 40) + "' cy='" + std::to_string(i % 24) + "' r='3'/>";
  }
  const std::string drawing = "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='24'>" + body + "</svg>";
  for (const char* name : {"/drawings/a.svg", "/drawings/b.svg"}) {
    fixture.storage.Put(name, {drawing.begin(), drawing.end()});
  }
  fixture.app.ViewDrawings(bn::reader::DrawingViewer::kFolder, true);
  fixture.app.Start();
  std::vector<std::uint64_t> pauses;
  std::uint64_t sinceAPause = 0;
  std::uint64_t longestRun = 0;
  for (int turn = 0; turn < 3000; ++turn) {
    const std::uint64_t pause = fixture.app.PauseUs();
    pauses.push_back(pause);
    sinceAPause = pause == 0 ? sinceAPause + 1000 : 0;
    longestRun = std::max(longestRun, sinceAPause);
    // Each turn takes a millisecond, and the panel is done at once.
    fixture.clock.now += pause + 1000;
    fixture.display.busy = false;
    fixture.app.Tick();
  }
  EXPECT_GT(std::count(pauses.begin(), pauses.end(), 0U), 10);
  EXPECT_LE(longestRun, bn::reader::App::kLongestRunUs);
  // At rest at the end: both shown, the tour over.
  EXPECT_FALSE(fixture.app.WorkWaiting());
  EXPECT_TRUE(std::all_of(pauses.end() - 50, pauses.end(),
                          [](std::uint64_t pause) { return pause == bn::reader::App::kTurnPauseUs; }));
}

TEST(App, ShowsTextInAFontsImageInPlaceOfTheTestPattern) {
  // A face with the letters a to e, and a card with a line of them.
  bn::test::ImageFace face = bn::test::EmptyFace(0, 0, 20);
  for (std::uint32_t letter = 'a'; letter <= 'e'; ++letter) {
    face.glyphs[letter] = bn::test::Block(40, 8, 10, 2);
  }
  face.glyphs[' '] = bn::test::Blank(20);
  const std::vector<std::uint8_t> image = bn::test::FontImageBytes({face});
  AppFixture fixture;
  fixture.fonts = bn::core::MemoryByteMap(image);
  fixture.display.present = true;
  fixture.display.levels = 4;
  fixture.display.Resize({.width = 200, .height = 120});
  const std::string text = "abc de\n\ncab\n";
  fixture.storage.Put("/layout/paragraphs.txt", {text.begin(), text.end()});
  EXPECT_EQ(fixture.app.TextsViewer(), nullptr);
  fixture.app.ViewText();
  ASSERT_NE(fixture.app.TextsViewer(), nullptr);
  fixture.app.Start();
  // No pause while the page is set and written; then the panel's refresh,
  // which is over at once here, and the gray pass after it.
  EXPECT_TRUE(fixture.app.WorkWaiting());
  EXPECT_EQ(fixture.app.PauseUs(), 0U);
  for (int turn = 0; turn < 200; ++turn) {
    fixture.clock.now += 1000;
    fixture.display.busy = false;
    fixture.app.Tick();
  }
  EXPECT_EQ(fixture.app.TextsViewer()->Shown(), 1U);
  EXPECT_EQ(fixture.app.TextsViewer()->Glyphs().size(), 8U);
  EXPECT_TRUE(fixture.app.TextsViewer()->GrayShown());
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kRefresh), 1U);
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kGrayRefresh), 1U);
  EXPECT_FALSE(fixture.app.WorkWaiting());
  EXPECT_EQ(fixture.app.PauseUs(), bn::reader::App::kTurnPauseUs);
  // The letters are dark gray: two levels of ink of three.
  EXPECT_EQ(fixture.display.LevelAt(bn::reader::TextViewer::kMargin + 2, bn::reader::TextViewer::kMargin + 8), 1);
  EXPECT_EQ(fixture.display.LevelAt(2, 2), 3);
}

TEST(App, ShowsTheTestPatternAndLetsTheButtonsStepThroughIt) {
  AppFixture fixture;
  fixture.display.present = true;
  fixture.app.Start();
  EXPECT_TRUE(fixture.input.started);
  EXPECT_EQ(fixture.app.Viewer().Picture(), 1);
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kRefresh), 1u);
  fixture.display.busy = false;
  fixture.RunFor(100'000);

  // A press, and its release, which changes nothing.
  fixture.input.Press(bn::core::Button::kDown, fixture.clock.now);
  fixture.input.Release(bn::core::Button::kDown, fixture.clock.now + 80'000);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.app.Viewer().Picture(), 2);
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kRefresh), 2u);
  EXPECT_TRUE(fixture.input.events.empty());

  fixture.display.busy = false;
  fixture.RunFor(5'100'000);
  const std::string metrics = fixture.Metrics();
  EXPECT_NE(metrics.find("panel_ok=1["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("panel_picture=2["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("input_ok=1["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("button_presses="), std::string::npos) << metrics;

  // The press is in the trace, from the button to the refresh.
  int presses = 0;
  int sweeps = 0;
  for (const auto& [id, arg] : fixture.TraceRecords()) {
    presses += id == static_cast<std::uint16_t>(bn::reader::kSpanPressToRefresh) ? 1 : 0;
    sweeps += id == static_cast<std::uint16_t>(bn::reader::kSpanPanelSweep) ? 1 : 0;
  }
  EXPECT_EQ(presses, 1);
  EXPECT_EQ(sweeps, 3);  // The first picture, the second got ready ahead of the press, and the third after it.
}

TEST(App, CanHaveThePanelsPlanesCheckedAsItRuns) {
  AppFixture fixture;
  fixture.display.present = true;
  fixture.app.CheckPanelPlanes();
  fixture.app.Start();
  fixture.display.busy = false;
  fixture.RunFor(1'000'000);
  EXPECT_EQ(fixture.display.reads, 12) << "both planes, a band at a time";
  fixture.RunFor(5'000'000);
  EXPECT_NE(fixture.Metrics().find("plane_checks="), std::string::npos) << fixture.Metrics();
  EXPECT_NE(fixture.Metrics().find("planes_unexpected="), std::string::npos) << fixture.Metrics();

  // Without being told to, it reads nothing.
  AppFixture plain;
  plain.display.present = true;
  plain.app.Start();
  plain.display.busy = false;
  plain.RunFor(1'000'000);
  EXPECT_EQ(plain.display.reads, 0);
}

TEST(App, GetsStorageReadyAndSaysWhetherItIs) {
  AppFixture fixture;
  fixture.app.Start();
  EXPECT_TRUE(fixture.storage.started);
  fixture.RunFor(5'100'000);
  EXPECT_NE(fixture.Metrics().find("storage_ok=1["), std::string::npos) << fixture.Metrics();
  bool traced = false;
  for (const auto& [id, arg] : fixture.TraceRecords()) {
    traced = traced || (id == static_cast<std::uint16_t>(bn::reader::kSpanStorageStart) && arg == 0);
  }
  EXPECT_TRUE(traced);
  // It does nothing with it unless asked to.
  EXPECT_EQ(fixture.storage.changes, 0);
  EXPECT_EQ(fixture.app.StorageChecker(), nullptr);
  EXPECT_EQ(fixture.app.BookChecker(), nullptr);

  AppFixture noCard;
  noCard.storage.present = false;
  noCard.app.Start();
  noCard.RunFor(5'100'000);
  EXPECT_NE(noCard.Metrics().find("storage_ok=0["), std::string::npos) << noCard.Metrics();
}

TEST(App, CanPutStorageThroughItsPacesAtStartAndAtEachPressOfPower) {
  AppFixture fixture;
  fixture.app.CheckStorage();
  fixture.app.Start();
  ASSERT_NE(fixture.app.StorageChecker(), nullptr);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kRunning);
  fixture.RunFor(15'000'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kPassed);
  EXPECT_NE(fixture.Metrics().find("storage_check=1["), std::string::npos) << fixture.Metrics();
  const int changesInOneRun = fixture.storage.changes;
  EXPECT_GT(changesInOneRun, 64);
  EXPECT_FALSE(fixture.storage.Has("/biblionaut-check")) << "it clears up after itself";

  // Power runs it again.
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kRunning);
  fixture.RunFor(15'000'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kPassed);
  EXPECT_EQ(fixture.storage.changes, 2 * changesInOneRun);
}

TEST(App, LooksForACardAgainWhenPowerIsPressed) {
  AppFixture fixture;
  fixture.storage.present = false;
  fixture.app.CheckStorage();
  fixture.app.Start();
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kIdle) << "no card to check";

  // Still none: the check says so.
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kFailed);
  EXPECT_EQ(fixture.app.StorageChecker()->FailedStatus(), bn::core::StorageStatus::kAbsent);

  // A card goes in.
  fixture.storage.present = true;
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(15'000'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kPassed);
  EXPECT_NE(fixture.Metrics().find("storage_ok=1["), std::string::npos) << fixture.Metrics();
}

TEST(App, CanHaveTheBooksOnTheCardReadAtStartAndAtEachPressOfPower) {
  using BookState = bn::reader::BookCheck::State;
  AppFixture fixture;
  bn::test::ZipBuilder builder;
  builder.Add("mimetype", {'a', 'b', 'c'});
  builder.Add("OEBPS/chapter.xhtml", std::vector<std::uint8_t>(20'000, 'x'), {.deflate = true});
  fixture.storage.Put("/books/a.epub", builder.Finish());
  fixture.app.CheckBooks();
  fixture.app.Start();
  ASSERT_NE(fixture.app.BookChecker(), nullptr);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kRunning);
  fixture.RunFor(5'100'000);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kPassed);
  EXPECT_EQ(fixture.app.BookChecker()->Totals().bytesOut, 20'003u);
  EXPECT_NE(fixture.Metrics().find("book_check=1["), std::string::npos) << fixture.Metrics();
  EXPECT_EQ(fixture.storage.changes, 0) << "it writes nothing";

  // What it found goes out as text.
  const auto passedLines = [&fixture] {
    int count = 0;
    for (const StreamItem& item : fixture.ItemsOfType(FrameType::kText)) {
      count += item.Text() == "book check: passed. 1 book, 2 entries, 20003 bytes out of " +
                                  std::to_string(fixture.app.BookChecker()->Totals().bytesIn) + "."
                   ? 1
                   : 0;
    }
    return count;
  };
  EXPECT_EQ(passedLines(), 1);

  // Power runs it again.
  const int opensInOneRun = fixture.storage.opens;
  EXPECT_EQ(opensInOneRun, 4) << "the book is open twice, for its directory and for its entries, to look for its "
                                 "package, and twice again to be read";
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kPassed);
  EXPECT_EQ(fixture.storage.opens, 2 * opensInOneRun);
  EXPECT_EQ(passedLines(), 2);
}

TEST(App, LooksForACardAgainBeforeReadingTheBooksWhenPowerIsPressed) {
  using BookState = bn::reader::BookCheck::State;
  AppFixture fixture;
  bn::test::ZipBuilder builder;
  builder.Add("mimetype", {'a', 'b', 'c'});
  fixture.storage.Put("/books/a.epub", builder.Finish());
  fixture.storage.present = false;
  fixture.app.CheckBooks();
  fixture.app.Start();
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kIdle) << "no card to read from";

  // Still none: the check says so.
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kFailed);

  // A card goes in.
  fixture.storage.present = true;
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.BookChecker()->CurrentState(), BookState::kPassed);
  EXPECT_EQ(fixture.app.BookChecker()->Books(), 1u);
}

TEST(App, CanHaveTheLayoutCodeTimedAtStartAndAtEachPressOfPower) {
  using LayoutState = bn::reader::LayoutCheck::State;
  AppFixture fixture;
  const std::string text = "The lamp was lit before anyone had asked for it.\n\nAnd the harbour went on.\n";
  fixture.storage.Put(std::string(bn::reader::LayoutCheck::kTextPath), {text.begin(), text.end()});
  fixture.app.CheckLayout();
  fixture.app.Start();
  ASSERT_NE(fixture.app.LayoutChecker(), nullptr);
  EXPECT_EQ(fixture.app.LayoutChecker()->CurrentState(), LayoutState::kRunning);
  fixture.RunFor(5'100'000);
  EXPECT_EQ(fixture.app.LayoutChecker()->CurrentState(), LayoutState::kPassed);
  EXPECT_EQ(fixture.app.LayoutChecker()->PassFigures(0).paragraphs, 2U);
  // With no table on the card, the check is given American English's.
  EXPECT_TRUE(fixture.app.LayoutChecker()->HyphenatingBuiltIn());
  EXPECT_EQ(fixture.app.LayoutChecker()->Passes(), 3U);
  EXPECT_NE(fixture.Metrics().find("layout_check=1["), std::string::npos) << fixture.Metrics();
  EXPECT_EQ(fixture.storage.changes, 0) << "it writes nothing";

  const auto passedLines = [&fixture] {
    int count = 0;
    for (const StreamItem& item : fixture.ItemsOfType(FrameType::kText)) {
      count += item.Text() == "layout check: passed." ? 1 : 0;
    }
    return count;
  };
  EXPECT_EQ(passedLines(), 1);

  // Power runs it again.
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(200'000);
  EXPECT_EQ(fixture.app.LayoutChecker()->CurrentState(), LayoutState::kPassed);
  EXPECT_EQ(passedLines(), 2);
}

TEST(App, PutsTheDeviceToSleepWhenPowerIsPressed) {
  using Op = bn::test::FakeDisplay::Op;
  AppFixture fixture;
  fixture.display.present = true;
  fixture.power.canSleep = true;
  fixture.app.Start();
  fixture.display.busy = false;
  fixture.RunFor(100'000);

  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(50'000);
  // The voltages are on after the first picture: they go off first.
  EXPECT_EQ(fixture.display.Count(Op::kPowerOff), 1u);
  EXPECT_EQ(fixture.power.sleeps, 0);
  EXPECT_FALSE(fixture.app.Asleep());

  fixture.display.busy = false;
  fixture.RunFor(50'000);
  EXPECT_EQ(fixture.display.Count(Op::kSleep), 1u);
  EXPECT_EQ(fixture.app.Viewer().CurrentState(), bn::reader::PatternViewer::State::kAsleep);
  EXPECT_EQ(fixture.power.sleeps, 1);
  EXPECT_TRUE(fixture.app.Asleep());
  // The trace went out before it slept, the time from the press to sleep
  // among it.
  int traced = 0;
  for (const auto& [id, arg] : fixture.TraceRecords()) {
    traced += id == static_cast<std::uint16_t>(bn::reader::kSpanPressToSleep) ? 1 : 0;
  }
  EXPECT_EQ(traced, 1);

  // Its run is over: nothing more happens.
  const std::size_t sent = fixture.sink.received.size();
  const std::size_t calls = fixture.display.calls.size();
  fixture.input.Press(bn::core::Button::kDown, fixture.clock.now);
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(6'000'000);
  EXPECT_EQ(fixture.sink.received.size(), sent);
  EXPECT_EQ(fixture.display.calls.size(), calls);
  EXPECT_EQ(fixture.power.sleeps, 1);
}

TEST(App, LetsTheRefreshInProgressFinishBeforeSleeping) {
  AppFixture fixture;
  fixture.display.present = true;
  fixture.power.canSleep = true;
  fixture.app.Start();
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(1'000'000);
  EXPECT_EQ(fixture.power.sleeps, 0) << "the first picture is still on its way";

  fixture.display.busy = false;
  fixture.RunFor(50'000);
  fixture.display.busy = false;
  fixture.RunFor(50'000);
  EXPECT_EQ(fixture.power.sleeps, 1);
  EXPECT_EQ(fixture.display.refused, 0);
}

TEST(App, StaysAwakeWhereTheDeviceCannotSleep) {
  AppFixture fixture;
  fixture.display.present = true;
  fixture.app.Start();
  fixture.display.busy = false;
  fixture.RunFor(100'000);

  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.display.Count(bn::test::FakeDisplay::Op::kSleep), 0u);
  EXPECT_FALSE(fixture.app.Asleep());
  fixture.input.Press(bn::core::Button::kDown, fixture.clock.now);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.app.Viewer().Picture(), 2);
}

TEST(App, LeavesPowerToTheChecksWhenThereAreAny) {
  AppFixture fixture;
  fixture.power.canSleep = true;
  fixture.app.CheckStorage();
  fixture.app.Start();
  fixture.RunFor(15'000'000);
  ASSERT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kPassed);

  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.app.StorageChecker()->CurrentState(), bn::reader::StorageCheck::State::kRunning);
  EXPECT_EQ(fixture.power.sleeps, 0);
  EXPECT_FALSE(fixture.app.Asleep());
}

TEST(App, SleepsWhenThereIsNoPanelToPutToSleepFirst) {
  AppFixture fixture;  // The panel cannot be set up.
  fixture.power.canSleep = true;
  fixture.app.Start();
  fixture.RunFor(100'000);
  fixture.input.Press(bn::core::Button::kPower, fixture.clock.now);
  fixture.RunFor(100'000);
  EXPECT_EQ(fixture.power.sleeps, 1);
  EXPECT_TRUE(fixture.app.Asleep());
}

TEST(App, ReportsButtonsThatCannotBeRead) {
  AppFixture fixture;
  fixture.input.present = false;
  fixture.app.Start();
  fixture.RunFor(5'100'000);
  EXPECT_NE(fixture.Metrics().find("input_ok=0["), std::string::npos) << fixture.Metrics();
}

TEST(App, InstallsItsTracerWhileItLives) {
  {
    AppFixture fixture;
    fixture.app.Start();
    EXPECT_NE(bn::core::ActiveTracer(), nullptr);
  }
  EXPECT_EQ(bn::core::ActiveTracer(), nullptr);
}

TEST(App, TickSendsTheStartSpanAndAHeartbeatEverySecond) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.RunFor(3'500'000);

  // Start, getting storage ready, then the heartbeats.
  const auto records = fixture.TraceRecords();
  ASSERT_EQ(records.size(), 5u);
  EXPECT_EQ(records[0].first, kStartId);
  EXPECT_EQ(records[1].first, static_cast<std::uint16_t>(bn::reader::kSpanStorageStart));
  EXPECT_EQ(records[2], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 1}));
  EXPECT_EQ(records[3], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 2}));
  EXPECT_EQ(records[4], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 3}));
}

TEST(App, SendsMetricsEveryFiveSeconds) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.RunFor(4'900'000);
  EXPECT_TRUE(fixture.ItemsOfType(FrameType::kMetrics).empty());

  fixture.RunFor(5'200'000);
  const auto metrics = fixture.ItemsOfType(FrameType::kMetrics);
  ASSERT_FALSE(metrics.empty());
  // Two reports; each may span more than one frame, all with the same time.
  std::set<std::uint32_t> times;
  for (const StreamItem& item : metrics) {
    times.insert(item.TimeUs());
  }
  EXPECT_EQ(times.size(), 2u);
  EXPECT_NE(fixture.Metrics().find("app_ticks="), std::string::npos);
  EXPECT_NE(fixture.Metrics().find("trace_dropped=0[0..0]"), std::string::npos);
}

TEST(App, ReportsHeapAndStackLevelsWithTheMetrics) {
  AppFixture fixture;
  fixture.memory.heap =
      bn::core::HeapStats{.freeBytes = 120'000, .largestFreeBlock = 90'000, .lowestFreeBytes = 77'000};
  fixture.memory.stackHeadroom = 2'100;
  fixture.app.Start();
  fixture.RunFor(5'100'000);

  const std::string metrics = fixture.Metrics();
  EXPECT_NE(metrics.find("heap_free=120000["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("heap_largest_block=90000["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("heap_low_water=77000["), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("stack_headroom=2100["), std::string::npos) << metrics;
}

TEST(App, SamplesMemoryOncePerMetricsReportAndTracesIt) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.RunFor(4'900'000);
  EXPECT_EQ(fixture.memory.heapReads, 0);

  fixture.RunFor(5'300'000);
  EXPECT_EQ(fixture.memory.heapReads, 2);

  int sampleSpans = 0;
  for (const auto& [id, arg] : fixture.TraceRecords()) {
    sampleSpans += id == static_cast<std::uint16_t>(bn::reader::kSpanSampleMemory) ? 1 : 0;
  }
  EXPECT_EQ(sampleSpans, 2);
}

TEST(App, SendsBufferedTraceBeforeTheMetricsOfTheSameTick) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.RunFor(5'100'000);

  // The fifth heartbeat, the memory sample and the first metrics report fall
  // in the same tick. The trace records happened first and must arrive first.
  const std::vector<StreamItem> items = fixture.Items();
  std::size_t metricsAt = items.size();
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].isFrame && items[i].type == FrameType::kMetrics) {
      metricsAt = i;
      break;
    }
  }
  ASSERT_LT(metricsAt, items.size());

  std::vector<std::uint16_t> idsBefore;
  std::vector<std::uint32_t> argsBefore;
  for (std::size_t i = 0; i < metricsAt; ++i) {
    if (!items[i].isFrame || items[i].type != FrameType::kTrace) {
      continue;
    }
    const std::span<const std::uint8_t> payload(items[i].payload);
    for (std::size_t at = 4; at + bn::core::kWireRecordSize <= payload.size(); at += bn::core::kWireRecordSize) {
      idsBefore.push_back(bn::core::LoadLe16(payload.subspan(at + 12).first<2>()));
      argsBefore.push_back(bn::core::LoadLe32(payload.subspan(at + 8).first<4>()));
    }
  }
  // Start, storage, heartbeats 1 to 5, then the memory sample.
  ASSERT_EQ(idsBefore.size(), 8u);
  EXPECT_EQ(idsBefore[6], kHeartbeatId);
  EXPECT_EQ(argsBefore[6], 5u);
  EXPECT_EQ(idsBefore[7], static_cast<std::uint16_t>(bn::reader::kSpanSampleMemory));
}

TEST(App, KeepsTraceWhileTheSerialLineIsBlockedAndSendsItLater) {
  AppFixture fixture;
  fixture.app.Start();
  fixture.sink.full = true;
  fixture.RunFor(3'200'000);
  EXPECT_TRUE(fixture.ItemsOfType(FrameType::kTrace).empty());

  fixture.sink.full = false;
  fixture.app.Flush();
  const auto records = fixture.TraceRecords();
  ASSERT_EQ(records.size(), 5u);
  EXPECT_EQ(records[0].first, kStartId);
  EXPECT_EQ(records[4], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 3}));
}

TEST(App, DefaultServicesUseTheLinkedHardwarePort) {
  const bn::reader::Services services = bn::reader::DefaultServices();
  const std::uint64_t first = services.clock.NowUs();
  const std::uint64_t second = services.clock.NowUs();
  EXPECT_GE(second, first);
  EXPECT_TRUE(services.serial.Write({}));
  services.traceSync.Lock();
  services.traceSync.Unlock();
  // The desktop port has no fixed heap or task stacks to report on.
  EXPECT_FALSE(services.memory.Heap().has_value());
  EXPECT_FALSE(services.memory.StackHeadroom().has_value());
  // And, unless asked to invent one, no crash to report.
  EXPECT_EQ(services.crash.Saved(), nullptr);
  // Its panel is a simulation of the X4's.
  EXPECT_EQ(services.display.Size().width, 800u);
  // And it can sleep, in its way: the desktop application ends the run.
  EXPECT_TRUE(services.power.CanSleep());
  EXPECT_EQ(services.display.Size().height, 480u);
  // And nobody is pressing its buttons, unless a script says so.
  bn::core::InputEvent event{};
  EXPECT_TRUE(services.input.Start());
  EXPECT_FALSE(services.input.Next(event));
}

TEST(Version, IsNotEmpty) { EXPECT_GT(std::strlen(bn::reader::Version()), 0u); }

}  // namespace
