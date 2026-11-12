#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/crash.h"
#include "core/endian.h"
#include "core/frame.h"
#include "core/metrics.h"
#include "core/trace_stream.h"
#include "crash_fakes.h"
#include "reader/trace_ids.h"
#include "stream_fakes.h"
#include "trace/decoder.h"
#include "trace/input.h"
#include "trace/names.h"
#include "trace/output.h"
#include "trace_fakes.h"

namespace {

using bn::core::MakeSpanId;
using bn::core::TracePart;
using bn::reader::kInstantHeartbeat;
using bn::reader::kSpanAppStart;
using bn::trace_tool::Event;
using bn::trace_tool::EventKind;
using bn::trace_tool::StreamDecoder;

// A device in miniature: a tracer and a stream writing into a byte buffer.
template <std::size_t Capacity = 64>
struct Device {
  bn::test::TraceFixture<Capacity> trace;
  bn::test::FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};

  void Span(bn::core::SpanId id, std::uint64_t startUs, std::uint64_t durationUs, std::uint32_t arg = 0) {
    trace.clock.now = startUs;
    const std::uint32_t start = trace.tracer.Now();
    trace.clock.now = startUs + durationUs;
    trace.tracer.RecordSpan(id, start, arg);
  }

  void Instant(bn::core::SpanId id, std::uint64_t atUs, std::uint32_t arg = 0) {
    trace.clock.now = atUs;
    trace.tracer.RecordInstant(id, arg);
  }

  void PumpAll() {
    while (stream.Pump() > 0) {
    }
  }

  void Raw(std::string_view text) { sink.received.insert(sink.received.end(), text.begin(), text.end()); }

  // Sends all of a crash report, as the application does over several ticks.
  void SendCrash(const bn::core::CrashReport& report) {
    std::size_t offset = 0;
    while (const std::size_t sent = stream.SendCrashPiece(report, offset)) {
      offset += sent;
    }
  }
};

// A crash an ESP32-C3 could have left: a fault in flash code, with a short
// stack and two trace records leading up to it.
bn::core::CrashReport SmallCrashReport() {
  bn::core::CrashReport report{};
  report.arch = bn::core::CrashArch::kEsp32C3;
  report.timeUs = 12'345'678;
  report.bootsSince = 1;
  bn::core::SetCrashText(report.reason, "Load access fault");
  bn::core::SetCrashText(report.task, "main");
  bn::core::SetCrashText(report.version, "0.1.0-test");
  bn::core::SetCrashText(report.buildId, "1a2b3c4d5");
  report.registerCount = bn::core::kCrashMaxRegisters;
  report.registers[0] = 0x4200A1B2;   // mepc
  report.registers[1] = 0x4200C3D4;   // ra
  report.registers[2] = 0x3FC9F000;   // sp
  report.registers[34] = 5;           // mcause
  report.registers[35] = 0x00000010;  // mtval
  report.stackAddress = 0x3FC9F000;
  report.stackSize = 24;
  // A saved register, a return address into flash, a data pointer, a return
  // address into RAM code, an odd value in the flash range, and zero.
  const std::array<std::uint32_t, 6> words = {0x00000001, 0x4200C3D4, 0x3FC9F040, 0x40381234, 0x42001123, 0};
  for (std::size_t i = 0; i < words.size(); ++i) {
    bn::core::StoreLe32(std::span(report.stack).subspan(i * 4).first<4>(), words[i]);
  }
  report.traceCount = 2;
  report.trace[0] = bn::core::TraceRecord{.startUs = 12'000'000,
                                          .durationUs = 0,
                                          .arg = 12,
                                          .id = kInstantHeartbeat,
                                          .task = 0,
                                          .kind = bn::core::TraceKind::kInstant};
  report.trace[1] = bn::core::TraceRecord{.startUs = 12'300'000,
                                          .durationUs = 250,
                                          .arg = 0,
                                          .id = kSpanAppStart,
                                          .task = 1,
                                          .kind = bn::core::TraceKind::kSpan};
  return report;
}

std::vector<Event> Decode(std::span<const std::uint8_t> bytes, StreamDecoder& decoder) {
  decoder.Feed(bytes);
  decoder.Finish();
  return decoder.TakeEvents();
}

std::vector<Event> Decode(std::span<const std::uint8_t> bytes) {
  StreamDecoder decoder;
  return Decode(bytes, decoder);
}

std::span<const std::uint8_t> Bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST(StreamDecoder, DecodesEveryKindOfFrame) {
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  device.Span(kSpanAppStart, 1'000, 250);
  device.trace.sync.task = 3;
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.PumpAll();
  device.trace.clock.now = 1'500'000;
  device.stream.SendText("page 12 laid out");
  bn::core::MetricRegistry registry;
  bn::core::Counter reads("sd_reads", registry);
  reads.Add(7);
  device.trace.clock.now = 2'000'000;
  device.stream.SendMetrics(registry);

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 5u);

  EXPECT_EQ(events[0].kind, EventKind::kHello);
  EXPECT_EQ(events[0].boot, 1);
  EXPECT_EQ(events[0].arg, bn::trace_tool::kSupportedProtocol);
  EXPECT_EQ(events[0].text, "0.1.0-test");

  EXPECT_EQ(events[1].kind, EventKind::kSpan);
  EXPECT_EQ(events[1].id, kSpanAppStart);
  EXPECT_EQ(events[1].timeUs, 1'000u);
  EXPECT_EQ(events[1].durationUs, 250u);
  EXPECT_EQ(events[1].task, 0);
  EXPECT_EQ(events[1].boot, 1);

  EXPECT_EQ(events[2].kind, EventKind::kInstant);
  EXPECT_EQ(events[2].id, kInstantHeartbeat);
  EXPECT_EQ(events[2].timeUs, 1'000'000u);
  EXPECT_EQ(events[2].arg, 1u);
  EXPECT_EQ(events[2].task, 3);

  EXPECT_EQ(events[3].kind, EventKind::kText);
  EXPECT_EQ(events[3].timeUs, 1'500'000u);
  EXPECT_EQ(events[3].text, "page 12 laid out");

  EXPECT_EQ(events[4].kind, EventKind::kMetrics);
  EXPECT_EQ(events[4].timeUs, 2'000'000u);
  EXPECT_EQ(events[4].text, "sd_reads=7");

  EXPECT_EQ(decoder.Stats().frames, 4u);
  EXPECT_EQ(decoder.Stats().records, 2u);
  EXPECT_EQ(decoder.Stats().boots, 1);
  EXPECT_EQ(decoder.Stats().unsupportedFrames, 0u);
}

TEST(StreamDecoder, GivesTheSameEventsHoweverTheBytesArrive) {
  Device<> device;
  device.Raw("boot line\r\n");
  device.stream.SendHello("v");
  for (std::uint32_t i = 0; i < 20; ++i) {
    device.Span(kSpanAppStart, std::uint64_t{1'000} * (i + 1), 100, i);
  }
  device.PumpAll();
  device.stream.SendText("done");

  const std::vector<Event> whole = Decode(device.sink.received);

  StreamDecoder decoder;
  std::vector<Event> pieces;
  for (const std::uint8_t byte : device.sink.received) {
    decoder.Feed(std::span(&byte, 1));
    for (Event& event : decoder.TakeEvents()) {
      pieces.push_back(std::move(event));
    }
  }
  decoder.Finish();
  for (Event& event : decoder.TakeEvents()) {
    pieces.push_back(std::move(event));
  }

  ASSERT_EQ(pieces.size(), whole.size());
  ASSERT_EQ(whole.size(), 23u);
  for (std::size_t i = 0; i < whole.size(); ++i) {
    EXPECT_EQ(pieces[i].kind, whole[i].kind) << i;
    EXPECT_EQ(pieces[i].timeUs, whole[i].timeUs) << i;
    EXPECT_EQ(pieces[i].arg, whole[i].arg) << i;
    EXPECT_EQ(pieces[i].text, whole[i].text) << i;
  }
}

TEST(StreamDecoder, ShowsPlainTextBeforeTheFirstFrameLineByLineAsItArrives) {
  StreamDecoder decoder;
  decoder.Feed(Bytes("ESP-ROM:esp32c3\r\nI (312) main_task: Calling app_main()\r\npart"));
  std::vector<Event> events = decoder.TakeEvents();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, EventKind::kRawText);
  EXPECT_EQ(events[0].text, "ESP-ROM:esp32c3");
  EXPECT_EQ(events[1].text, "I (312) main_task: Calling app_main()");
  EXPECT_EQ(events[0].boot, 0);

  decoder.Feed(Bytes("ial line\n"));
  events = decoder.TakeEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].text, "partial line");
}

TEST(StreamDecoder, SeparatesPlainTextFromTheFramesAroundIt) {
  Device<> device;
  device.stream.SendHello("v");
  device.Raw("W (900) spi: something odd\n");
  device.stream.SendText("first");
  device.Raw("no line ending");
  device.stream.SendText("second");

  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[1].kind, EventKind::kRawText);
  EXPECT_EQ(events[1].text, "W (900) spi: something odd");
  EXPECT_EQ(events[2].text, "first");
  EXPECT_EQ(events[3].kind, EventKind::kRawText);
  EXPECT_EQ(events[3].text, "no line ending");
  EXPECT_EQ(events[4].text, "second");
}

TEST(StreamDecoder, ShowsALongCrashDumpThatNeverEndsInADelimiter) {
  Device<> device;
  device.stream.SendHello("v");
  std::string dump;
  for (int i = 0; i < 40; ++i) {
    dump += "Guru Meditation line " + std::to_string(i) + " 0x4200abcd 0x3fc80000\r\n";
  }
  dump += "Rebooting...";
  device.Raw(dump);

  StreamDecoder decoder;
  decoder.Feed(device.sink.received);
  // Most of it is shown before the stream ends: it is too long to be a frame.
  const std::size_t beforeFinish = decoder.TakeEvents().size();
  EXPECT_GT(beforeFinish, 20u);

  decoder.Finish();
  const std::vector<Event> rest = decoder.TakeEvents();
  ASSERT_FALSE(rest.empty());
  EXPECT_EQ(rest.back().text, "Rebooting...");
  EXPECT_EQ(decoder.Stats().rawTextLines, 41u);
}

TEST(StreamDecoder, TreatsADamagedFrameAsTextAndCarriesOn) {
  Device<> device;
  device.stream.SendText("one");
  const std::size_t firstSize = device.sink.received.size();
  device.stream.SendText("two");
  device.stream.SendText("three");
  std::uint8_t& damaged = device.sink.received[firstSize + 6];  // Inside the second frame.
  damaged = static_cast<std::uint8_t>(damaged ^ 0x20U);

  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].kind, EventKind::kText);
  EXPECT_EQ(events[0].text, "one");
  EXPECT_EQ(events[1].kind, EventKind::kRawText);
  EXPECT_EQ(events[2].kind, EventKind::kText);
  EXPECT_EQ(events[2].text, "three");
}

TEST(StreamDecoder, KeepsTimesGrowingWhenTheDeviceClockWraps) {
  Device<> device;
  device.stream.SendHello("v");
  device.Instant(kInstantHeartbeat, 0xFFFF'FF00ull, 1);
  device.Instant(kInstantHeartbeat, 0x1'0000'0100ull, 2);
  device.Span(kSpanAppStart, 0x1'0000'0200ull, 0x100, 3);
  device.PumpAll();
  // About 27 minutes later. (Two items more than 35 minutes apart cannot be
  // ordered from 32-bit times; the once-a-second heartbeat prevents that.)
  device.trace.clock.now = 0x1'6000'0300ull;
  device.stream.SendText("much later");

  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[1].timeUs, 0xFFFF'FF00ull);
  EXPECT_EQ(events[2].timeUs, 0x1'0000'0100ull);
  EXPECT_EQ(events[3].timeUs, 0x1'0000'0200ull);
  EXPECT_EQ(events[3].durationUs, 0x100u);
  EXPECT_EQ(events[4].timeUs, 0x1'6000'0300ull);
}

TEST(StreamDecoder, PlacesASpanByItsEndSoNestedSpansKeepTheirStarts) {
  Device<> device;
  device.stream.SendHello("v");
  // Inner ends first and is recorded first; outer started earlier.
  device.Span(MakeSpanId(TracePart::kLayout, 2), 5'000, 300);
  device.Span(MakeSpanId(TracePart::kLayout, 1), 4'000, 2'000);
  device.PumpAll();

  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[1].timeUs, 5'000u);
  EXPECT_EQ(events[1].durationUs, 300u);
  EXPECT_EQ(events[2].timeUs, 4'000u);
  EXPECT_EQ(events[2].durationUs, 2'000u);
}

TEST(StreamDecoder, ANewHelloStartsANewBootWithItsOwnClock) {
  Device<> device;
  device.stream.SendHello("first");
  device.Instant(kInstantHeartbeat, 9'000'000, 9);
  device.PumpAll();
  device.stream.SendHello("second");
  device.Instant(kInstantHeartbeat, 1'000, 1);
  device.PumpAll();

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[1].boot, 1);
  EXPECT_EQ(events[1].timeUs, 9'000'000u);
  EXPECT_EQ(events[2].kind, EventKind::kHello);
  EXPECT_EQ(events[2].boot, 2);
  EXPECT_EQ(events[3].boot, 2);
  EXPECT_EQ(events[3].timeUs, 1'000u);
  EXPECT_EQ(decoder.Stats().boots, 2);
}

TEST(StreamDecoder, ReportsRecordsTheDeviceLost) {
  Device<4> device;
  device.stream.SendHello("v");
  for (std::uint32_t i = 0; i < 6; ++i) {
    device.Instant(kInstantHeartbeat, std::uint64_t{1'000} * (i + 1), i);
  }
  device.PumpAll();

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 6u);
  EXPECT_EQ(events[1].kind, EventKind::kDropped);
  EXPECT_EQ(events[1].arg, 2u);
  EXPECT_EQ(events[2].arg, 2u);  // The first record that survived.
  EXPECT_EQ(decoder.Stats().droppedRecords, 2u);
}

TEST(StreamDecoder, SkipsFramesOfAProtocolItDoesNotKnow) {
  bn::test::FakeSink sink;
  std::array<std::uint8_t, 64> buffer{};
  bn::core::FrameWriter hello(bn::core::FrameType::kHello, buffer);
  hello.Put(std::uint8_t{9});
  hello.Put(Bytes("future"));
  ASSERT_TRUE(sink.Write(hello.Finish()));

  bn::test::TraceFixture<8> trace;
  bn::core::TraceStream stream(trace.tracer, sink);
  stream.SendText("not understood");

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(sink.received, decoder);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, EventKind::kHello);
  EXPECT_EQ(events[0].arg, 9u);
  EXPECT_EQ(decoder.Stats().unsupportedFrames, 1u);
  EXPECT_NE(bn::trace_tool::FormatEvent(events[0]).find("frames are skipped"), std::string::npos);
}

TEST(StreamDecoder, PutsACrashReportBackTogetherFromItsPieces) {
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  const bn::core::CrashReport report = bn::test::SampleCrashReport();
  device.SendCrash(report);
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.PumpAll();

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[1].kind, EventKind::kCrash);
  EXPECT_EQ(events[1].boot, 1);
  EXPECT_EQ(events[1].text, "Load access fault");
  ASSERT_NE(events[1].crash, nullptr);
  EXPECT_EQ(events[1].crash->bootsSince, 3u);
  EXPECT_EQ(events[1].crash->timeUs, report.timeUs);
  EXPECT_EQ(events[1].crash->registers, report.registers);
  EXPECT_EQ(events[1].crash->stack, report.stack);
  EXPECT_EQ(events[1].crash->trace[31].arg, report.trace[31].arg);
  EXPECT_EQ(events[2].kind, EventKind::kInstant);

  EXPECT_EQ(decoder.Stats().crashReports, 1u);
  EXPECT_EQ(decoder.Stats().damagedCrashReports, 0u);
  EXPECT_EQ(decoder.Stats().unsupportedFrames, 0u);
}

TEST(StreamDecoder, DoesNotShowACrashReportWithAPieceMissing) {
  const bn::core::CrashReport report = bn::test::SampleCrashReport();
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  std::size_t offset = 0;
  for (int piece = 0;; ++piece) {
    const std::size_t before = device.sink.received.size();
    const std::size_t sent = device.stream.SendCrashPiece(report, offset);
    if (sent == 0) {
      break;
    }
    offset += sent;
    if (piece == 2) {
      device.sink.received.resize(before);  // The third piece never arrives.
    }
  }

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, EventKind::kHello);
  EXPECT_EQ(decoder.Stats().crashReports, 0u);
  EXPECT_EQ(decoder.Stats().damagedCrashReports, 1u);
}

TEST(StreamDecoder, ARestartPartWayThroughACrashReportStartsItAgain) {
  const bn::core::CrashReport report = SmallCrashReport();
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  ASSERT_GT(device.stream.SendCrashPiece(report, 0), 0u);
  // The device restarts and sends the report from the beginning, now one
  // restart older.
  bn::core::CrashReport later = report;
  later.bootsSince = 2;
  device.stream.SendHello("0.1.0-test");
  device.SendCrash(later);

  StreamDecoder decoder;
  const std::vector<Event> events = Decode(device.sink.received, decoder);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[2].kind, EventKind::kCrash);
  EXPECT_EQ(events[2].boot, 2);
  EXPECT_EQ(events[2].crash->bootsSince, 2u);
  EXPECT_EQ(decoder.Stats().crashReports, 1u);
  EXPECT_EQ(decoder.Stats().damagedCrashReports, 1u);
}

TEST(StreamDecoder, IgnoresACrashPieceThatClaimsMoreThanItsReport) {
  // A piece whose offset and size run past the stated total is not trusted.
  std::array<std::uint8_t, 64> buffer{};
  bn::core::FrameWriter writer(bn::core::FrameType::kCrash, buffer);
  writer.PutU16(0);
  writer.PutU16(4);
  for (std::uint8_t i = 0; i < 8; ++i) {
    writer.Put(i);
  }
  const std::span<const std::uint8_t> frame = writer.Finish();

  StreamDecoder decoder;
  EXPECT_TRUE(Decode(frame, decoder).empty());
  EXPECT_EQ(decoder.Stats().crashReports, 0u);
  EXPECT_EQ(decoder.Stats().damagedCrashReports, 1u);
}

// Runs events through a filter the way the tool does.
std::vector<Event> Filter(bn::trace_tool::BootFilter& filter, const std::vector<Event>& events) {
  std::vector<Event> shown;
  for (const Event& event : events) {
    filter.Push(event, shown);
  }
  return shown;
}

constexpr std::string_view kRomBoot = "ESP-ROM:esp32c3-api1-20210207\r\nrst:0x15 (USB_UART_CHIP_RESET)\r\n";

TEST(BootFilter, DropsWhatCameBeforeTheRestartedDeviceAnnouncedItself) {
  // What the tool sees after restarting a device that had trace buffered:
  // half a frame, old records, then the new boot.
  Device<> device;
  device.Raw("\x90G\x03\x01");
  device.Instant(kInstantHeartbeat, 56'000'000, 56);
  device.PumpAll();
  device.stream.SendText("old log line");
  device.Raw(kRomBoot);
  device.stream.SendHello("v");
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.PumpAll();

  bn::trace_tool::BootFilter filter;
  const std::vector<Event> kept = Filter(filter, Decode(device.sink.received));
  ASSERT_EQ(kept.size(), 4u);
  EXPECT_EQ(kept[0].text, "ESP-ROM:esp32c3-api1-20210207");
  EXPECT_EQ(kept[1].text, "rst:0x15 (USB_UART_CHIP_RESET)");
  EXPECT_EQ(kept[2].kind, EventKind::kHello);
  EXPECT_EQ(kept[2].boot, 1);
  EXPECT_EQ(kept[3].arg, 1u);
  EXPECT_EQ(filter.Skipped(), 3u);
  EXPECT_EQ(filter.SkippedBoots(), 0);
  EXPECT_FALSE(filter.Waiting());
}

TEST(BootFilter, DropsALeftoverRunThatBeginsWithItsOwnHello) {
  // A device that last started with nothing reading it (after a flash, or
  // after a crash) still has that whole run buffered, hello first. It is
  // not the restart the tool asked for.
  Device<> device;
  device.stream.SendHello("old run");
  device.SendCrash(SmallCrashReport());
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.Instant(kInstantHeartbeat, 2'000'000, 2);
  device.PumpAll();
  device.Raw(kRomBoot);
  device.stream.SendHello("new run");
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.PumpAll();

  bn::trace_tool::BootFilter filter;
  const std::vector<Event> kept = Filter(filter, Decode(device.sink.received));
  ASSERT_EQ(kept.size(), 4u);
  EXPECT_EQ(kept[0].text, "ESP-ROM:esp32c3-api1-20210207");
  EXPECT_EQ(kept[0].boot, 0);  // Before the first hello that counts.
  EXPECT_EQ(kept[1].boot, 0);
  EXPECT_EQ(kept[2].kind, EventKind::kHello);
  EXPECT_EQ(kept[2].text, "new run");
  EXPECT_EQ(kept[2].boot, 1);  // Not 2: the run that was dropped is not counted.
  EXPECT_EQ(kept[3].kind, EventKind::kInstant);
  EXPECT_EQ(kept[3].boot, 1);

  EXPECT_EQ(filter.Skipped(), 4u);  // Hello, crash report, two heartbeats.
  EXPECT_EQ(filter.SkippedBoots(), 1);
  EXPECT_FALSE(filter.Waiting());
}

TEST(BootFilter, ShowsNothingFromAHelloOnUntilItKnowsWhichRunItWas) {
  Device<> device;
  device.stream.SendHello("v");
  device.stream.SendText("first");

  bn::trace_tool::BootFilter filter;
  EXPECT_TRUE(Filter(filter, Decode(device.sink.received)).empty());
  EXPECT_TRUE(filter.Waiting());
  EXPECT_EQ(filter.Skipped(), 0u);
}

TEST(BootFilter, StartsAtTheHelloWhenTheBootRomStaysSilent) {
  Device<> device;
  device.Instant(kInstantHeartbeat, 9'000'000, 9);
  device.PumpAll();
  device.stream.SendHello("v");
  device.stream.SendText("after");

  bn::trace_tool::BootFilter filter;
  EXPECT_TRUE(Filter(filter, Decode(device.sink.received)).empty());

  // No ROM line came, so the tool stops waiting: the hello was the boot.
  const std::vector<Event> released = filter.GiveUp();
  ASSERT_EQ(released.size(), 2u);
  EXPECT_EQ(released[0].kind, EventKind::kHello);
  EXPECT_EQ(released[0].boot, 1);
  EXPECT_EQ(released[1].text, "after");
  EXPECT_EQ(filter.Skipped(), 1u);  // The record from before the hello.
  EXPECT_EQ(filter.SkippedBoots(), 0);
  EXPECT_FALSE(filter.Waiting());

  // Nothing is held twice, and what comes later passes straight through.
  EXPECT_TRUE(filter.GiveUp().empty());
  Event later;
  later.kind = EventKind::kText;
  later.boot = 1;
  std::vector<Event> shown;
  filter.Push(later, shown);
  ASSERT_EQ(shown.size(), 1u);
  EXPECT_EQ(shown[0].boot, 1);
}

TEST(BootFilter, FindsTheRomLineStuckToTheEndOfAFrameCutShort) {
  // The restart cut the device off in the middle of a frame, so the ROM's
  // first line arrives on the end of those bytes with nothing between.
  Device<> device;
  device.stream.SendHello("old run");
  device.Raw("\x90G\x03");
  device.Raw(kRomBoot);
  device.stream.SendHello("new run");

  bn::trace_tool::BootFilter filter;
  const std::vector<Event> kept = Filter(filter, Decode(device.sink.received));
  ASSERT_EQ(kept.size(), 3u);
  EXPECT_EQ(kept[0].text, "ESP-ROM:esp32c3-api1-20210207");  // Without the stray bytes.
  EXPECT_EQ(kept[1].text, "rst:0x15 (USB_UART_CHIP_RESET)");
  EXPECT_EQ(kept[2].text, "new run");
  EXPECT_EQ(kept[2].boot, 1);
  EXPECT_EQ(filter.SkippedBoots(), 1);
}

TEST(BootFilter, PassesEverythingOnceToldToGiveUp) {
  bn::trace_tool::BootFilter filter;
  Event event;
  event.kind = EventKind::kText;
  std::vector<Event> shown;
  filter.Push(event, shown);
  EXPECT_TRUE(shown.empty());
  EXPECT_TRUE(filter.GiveUp().empty());
  filter.Push(event, shown);
  EXPECT_EQ(shown.size(), 1u);
  EXPECT_EQ(filter.Skipped(), 1u);
}

TEST(SanitizeText, KeepsReadableTextAndEscapesTheRest) {
  using bn::trace_tool::SanitizeText;
  EXPECT_EQ(SanitizeText(Bytes("plain text\twith a tab")), "plain text\twith a tab");
  EXPECT_EQ(SanitizeText(Bytes("caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x93\x96")),
            "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x93\x96");
  EXPECT_EQ(SanitizeText(Bytes("a\x01"
                               "b\x7F")),
            "a\\x01b\\x7F");
  EXPECT_EQ(SanitizeText(Bytes("\xFF\xC3")), "\\xFF\\xC3");           // Not UTF-8; a sequence cut short.
  EXPECT_EQ(SanitizeText(Bytes("\xC0\xAF")), "\\xC0\\xAF");           // Overlong encoding.
  EXPECT_EQ(SanitizeText(Bytes("\xED\xA0\x80")), "\\xED\\xA0\\x80");  // A surrogate.
}

TEST(Names, DescribesKnownIdsAndNumbersUnknownOnes) {
  const bn::trace_tool::SpanInfo start = bn::trace_tool::Describe(kSpanAppStart);
  EXPECT_TRUE(start.known);
  EXPECT_EQ(start.part, "reader");
  EXPECT_EQ(start.name, "app_start");
  EXPECT_TRUE(start.argument.empty());

  const bn::trace_tool::SpanInfo heartbeat = bn::trace_tool::Describe(kInstantHeartbeat);
  EXPECT_EQ(heartbeat.name, "heartbeat");
  EXPECT_EQ(heartbeat.argument, "uptime_s");

  const bn::trace_tool::SpanInfo unknown = bn::trace_tool::Describe(MakeSpanId(TracePart::kDraw, 200));
  EXPECT_FALSE(unknown.known);
  EXPECT_EQ(unknown.part, "draw");
  EXPECT_EQ(unknown.name, "#200");

  EXPECT_GE(bn::trace_tool::KnownSpanCount(), 2u);
}

TEST(FormatEvent, WritesOneReadableLinePerEvent) {
  using bn::trace_tool::FormatEvent;
  Event event;
  event.boot = 1;

  event.kind = EventKind::kHello;
  event.arg = bn::trace_tool::kSupportedProtocol;
  event.text = "0.1.0";
  EXPECT_EQ(FormatEvent(event), "=== boot 1: 0.1.0 (stream protocol 2) ===");

  event.kind = EventKind::kSpan;
  event.id = kSpanAppStart;
  event.timeUs = 5'017'809;
  event.durationUs = 158;
  event.arg = 0;
  event.text.clear();
  EXPECT_EQ(FormatEvent(event), "[     5.017809] task 0  reader/app_start  158 us");

  event.durationUs = 12'345;
  EXPECT_EQ(FormatEvent(event), "[     5.017809] task 0  reader/app_start  12.345 ms");
  event.durationUs = 1'567'000;
  EXPECT_EQ(FormatEvent(event), "[     5.017809] task 0  reader/app_start  1.567 s");

  event.kind = EventKind::kInstant;
  event.id = kInstantHeartbeat;
  event.task = bn::core::kTraceTaskInterrupt;
  event.arg = 5;
  EXPECT_EQ(FormatEvent(event), "[     5.017809] interrupts  reader/heartbeat uptime_s=5");

  event.id = MakeSpanId(TracePart::kEpub, 4);
  event.task = 2;
  EXPECT_EQ(FormatEvent(event), "[     5.017809] task 2  epub/#4 arg=5");

  event.kind = EventKind::kText;
  event.text = "hello";
  EXPECT_EQ(FormatEvent(event), "[     5.017809] log  hello");
  event.kind = EventKind::kMetrics;
  event.text = "a=1";
  EXPECT_EQ(FormatEvent(event), "[     5.017809] metrics  a=1");
  event.kind = EventKind::kRawText;
  event.text = "boot";
  EXPECT_EQ(FormatEvent(event), "              | boot");
  event.kind = EventKind::kDropped;
  event.arg = 3;
  EXPECT_EQ(FormatEvent(event), "[     5.017809] !! 3 trace records were lost on the device before this point");
}

TEST(FormatEvent, WritesACrashReportAsABlockOfLines) {
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  device.SendCrash(SmallCrashReport());
  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 2u);

  const std::string text = bn::trace_tool::FormatEvent(events[1]);
  const auto has = [&text](std::string_view piece) { return text.find(piece) != std::string::npos; };
  // The firmware is named before anything that holds an address.
  EXPECT_TRUE(
      text.starts_with("=== crash report: the run before this one ended in a crash ===\n"
                       "  firmware   0.1.0-test  (ELF 1a2b3c4d5)\n"
                       "  reason     Load access fault\n"))
      << text;
  EXPECT_TRUE(has("\n  when       12.345678 s after it started, in task main\n")) << text;
  EXPECT_TRUE(has("  registers  mepc    0x4200a1b2  ra      0x4200c3d4  sp      0x3fc9f000  gp      0x00000000\n"))
      << text;
  EXPECT_TRUE(has("mcause  0x00000005  mtval   0x00000010\n")) << text;

  // Only even values in a code range are offered as return addresses.
  EXPECT_TRUE(
      has("  stack      24 bytes saved from 0x3fc9f000; values in them that could be return addresses:\n"
          "             sp+0x004  0x4200c3d4\n"
          "             sp+0x00c  0x40381234\n"
          "  trace "))
      << text;

  // The trace is placed in time by counting back from the crash.
  EXPECT_TRUE(
      has("  trace      the last 2 records before the crash:\n"
          "             [    12.000000] task 0  reader/heartbeat uptime_s=12\n"
          "             [    12.300000] task 1  reader/app_start  250 us\n"))
      << text;
  EXPECT_TRUE(text.ends_with("==="));
  EXPECT_NE(text.back(), '\n');
}

TEST(FormatEvent, SaysHowManyRestartsAgoAnOlderCrashWasAndCopesWithMissingParts) {
  bn::core::CrashReport report{};
  report.bootsSince = 4;
  bn::core::SetCrashText(report.reason, "abort() was called at PC 0x42001234");
  Device<> device;
  device.SendCrash(report);
  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 1u);

  const std::string text = bn::trace_tool::FormatEvent(events[0]);
  const auto has = [&text](std::string_view piece) { return text.find(piece) != std::string::npos; };
  EXPECT_TRUE(text.starts_with("=== crash report: 4 restarts ago ended in a crash ===\n")) << text;
  EXPECT_TRUE(has("  firmware   (not recorded)\n")) << text;
  EXPECT_TRUE(has("in task (unknown)\n")) << text;
  EXPECT_TRUE(has("  stack      0 bytes saved from 0x00000000; nothing in them looks like a return address\n")) << text;
  EXPECT_TRUE(has("  trace      none recorded before the crash\n")) << text;
}

TEST(FormatEvent, PlacesCrashTraceCorrectlyWhenTheClockWrappedBeforeTheCrash) {
  // The crash came 5 seconds after the 32-bit clock wrapped; the record is
  // from 2 seconds before the wrap.
  bn::core::CrashReport report{};
  report.arch = bn::core::CrashArch::kEsp32C3;
  report.bootsSince = 1;
  report.timeUs = (std::uint64_t{1} << 32U) + 5'000'000;
  report.traceCount = 1;
  report.trace[0] = bn::core::TraceRecord{.startUs = 0xFFFFFFFFU - 2'000'000 + 1,
                                          .durationUs = 0,
                                          .arg = 4292,
                                          .id = kInstantHeartbeat,
                                          .task = 0,
                                          .kind = bn::core::TraceKind::kInstant};
  Device<> device;
  device.SendCrash(report);
  const std::vector<Event> events = Decode(device.sink.received);
  ASSERT_EQ(events.size(), 1u);
  const std::string text = bn::trace_tool::FormatEvent(events[0]);
  // 2^32 us is 4294.967296 s; two seconds before that.
  EXPECT_NE(text.find("[  4292.967296] task 0  reader/heartbeat uptime_s=4292"), std::string::npos) << text;
}

TEST(IsCodeAddress, KnowsWhereAnEsp32C3RunsCodeFrom) {
  using bn::core::CrashArch;
  using bn::trace_tool::IsCodeAddress;
  EXPECT_TRUE(IsCodeAddress(CrashArch::kEsp32C3, 0x42000020));   // Flash.
  EXPECT_TRUE(IsCodeAddress(CrashArch::kEsp32C3, 0x40380000));   // Code in RAM.
  EXPECT_TRUE(IsCodeAddress(CrashArch::kEsp32C3, 0x40000000));   // ROM.
  EXPECT_FALSE(IsCodeAddress(CrashArch::kEsp32C3, 0x42000021));  // Odd.
  EXPECT_FALSE(IsCodeAddress(CrashArch::kEsp32C3, 0x3FC90000));  // Data.
  EXPECT_FALSE(IsCodeAddress(CrashArch::kEsp32C3, 0x42800000));  // Past the end of flash.
  EXPECT_FALSE(IsCodeAddress(CrashArch::kEsp32C3, 0));
  EXPECT_FALSE(IsCodeAddress(CrashArch::kUnknown, 0x42000020));
}

TEST(JsonEscape, EscapesWhatJsonRequires) {
  EXPECT_EQ(bn::trace_tool::JsonEscape("plain"), "plain");
  EXPECT_EQ(bn::trace_tool::JsonEscape("a \"quoted\" \\ path"), "a \\\"quoted\\\" \\\\ path");
  EXPECT_EQ(bn::trace_tool::JsonEscape("tab\tnewline\n"), "tab\\tnewline\\n");
  EXPECT_EQ(bn::trace_tool::JsonEscape(std::string_view("\x01", 1)), "\\u0001");
  EXPECT_EQ(bn::trace_tool::JsonEscape("caf\xC3\xA9"), "caf\xC3\xA9");
}

TEST(PerfettoWriter, WritesSlicesMarkersCountersAndNames) {
  Device<> device;
  device.Raw("early \"boot\" text\n");
  device.stream.SendHello("0.1.0-test");
  device.Span(kSpanAppStart, 1'000, 250);
  device.Instant(kInstantHeartbeat, 1'000'000, 1);
  device.PumpAll();
  device.trace.clock.now = 1'500'000;
  device.stream.SendText("page 12");
  bn::core::MetricRegistry registry;
  bn::core::Counter reads("sd_reads", registry);
  bn::core::Gauge heap("heap_free", registry);
  const bn::core::Gauge unset("never_set", registry);
  reads.Add(7);
  heap.Set(91'000);
  device.trace.clock.now = 2'000'000;
  device.stream.SendMetrics(registry);

  bn::trace_tool::PerfettoWriter writer;
  for (const Event& event : Decode(device.sink.received)) {
    writer.Add(event);
  }
  std::ostringstream out;
  writer.Write(out);
  const std::string json = out.str();

  const auto has = [&json](std::string_view piece) { return json.find(piece) != std::string::npos; };
  EXPECT_TRUE(json.starts_with("{\"displayTimeUnit\":\"ms\",\"traceEvents\":["));
  EXPECT_TRUE(json.ends_with("\n]}\n"));

  // Boot 0 holds what came before the hello; boot 1 is process 2.
  // (These two need a raw-string delimiter: their text contains )" itself.)
  EXPECT_TRUE(
      has(R"j({"ph":"M","name":"process_name","pid":1,"args":{"name":"Biblionaut (before the first hello)"}})j"));
  EXPECT_TRUE(has(R"j({"ph":"M","name":"process_name","pid":2,"args":{"name":"Biblionaut 0.1.0-test (boot 1)"}})j"));
  EXPECT_TRUE(has(R"({"ph":"M","name":"thread_name","pid":2,"tid":0,"args":{"name":"task 0"}})"));
  EXPECT_TRUE(has(R"({"ph":"M","name":"thread_name","pid":2,"tid":1000,"args":{"name":"log"}})"));
  EXPECT_TRUE(has(R"({"ph":"M","name":"thread_name","pid":1,"tid":1001,"args":{"name":"console"}})"));

  EXPECT_TRUE(
      has(R"({"ph":"X","name":"app_start","cat":"reader","pid":2,"tid":0,"ts":1000,"dur":250,"args":{"arg":0}})"));
  EXPECT_TRUE(has(
      R"({"ph":"i","name":"heartbeat","cat":"reader","pid":2,"tid":0,"ts":1000000,"s":"t","args":{"uptime_s":1}})"));
  EXPECT_TRUE(has(R"({"ph":"i","s":"t","name":"page 12","cat":"log","pid":2,"tid":1000,"ts":1500000})"));
  EXPECT_TRUE(has(R"({"ph":"i","s":"t","name":"early \"boot\" text","cat":"console","pid":1,"tid":1001,"ts":0})"));
  EXPECT_TRUE(has(R"({"ph":"C","name":"sd_reads","pid":2,"ts":2000000,"args":{"value":7}})"));
  EXPECT_TRUE(has(R"({"ph":"C","name":"heap_free","pid":2,"ts":2000000,"args":{"value":91000}})"));
  EXPECT_FALSE(has("never_set"));
}

TEST(PerfettoWriter, MarksACrashReportOnTheRunThatDeliveredIt) {
  Device<> device;
  device.stream.SendHello("0.1.0-test");
  bn::core::CrashReport report = SmallCrashReport();
  bn::core::SetCrashText(report.reason, "assert failed: \"x\"");
  device.SendCrash(report);

  bn::trace_tool::PerfettoWriter writer;
  for (const Event& event : Decode(device.sink.received)) {
    writer.Add(event);
  }
  std::ostringstream out;
  writer.Write(out);
  const std::string json = out.str();
  const auto has = [&json](std::string_view piece) { return json.find(piece) != std::string::npos; };

  EXPECT_TRUE(has(R"({"ph":"M","name":"thread_name","pid":2,"tid":1000,"args":{"name":"log"}})")) << json;
  EXPECT_TRUE(has(R"j({"ph":"i","s":"p","name":"crash report: assert failed: \"x\"","cat":"crash","pid":2,"tid":1000,)j"
                  R"j("ts":0,"args":{"reason":"assert failed: \"x\"","firmware":"0.1.0-test","elf":"1a2b3c4d5",)j"
                  R"j("task":"main","restarts_since":1,"crashed_at_us":12345678,"pc":"0x4200a1b2"}})j"))
      << json;
}

TEST(PerfettoWriter, AnEmptyTraceIsStillValid) {
  const bn::trace_tool::PerfettoWriter writer;
  std::ostringstream out;
  writer.Write(out);
  EXPECT_EQ(out.str(), "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n\n]}\n");
}

// Reads from `input` until `wanted` bytes have arrived or it gives up.
std::vector<std::uint8_t> ReadBytes(bn::trace_tool::Input& input, std::size_t wanted) {
  std::vector<std::uint8_t> received;
  std::array<std::uint8_t, 256> buffer{};
  for (int attempts = 0; attempts < 50 && received.size() < wanted; ++attempts) {
    std::size_t count = 0;
    const auto result = input.Read(buffer, 100, count);
    if (result == bn::trace_tool::Input::Result::kEnd) {
      break;
    }
    received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
  }
  return received;
}

TEST(Input, ReadsAFileToItsEnd) {
  std::array<char, 64> path{};
  std::snprintf(path.data(), path.size(), "/tmp/bn_trace_input_XXXXXX");
  const int fd = mkstemp(path.data());
  ASSERT_GE(fd, 0);
  const std::array<std::uint8_t, 6> content{0, 1, 2, 0, 0xFF, 0x0D};
  ASSERT_EQ(write(fd, content.data(), content.size()), static_cast<ssize_t>(content.size()));
  close(fd);

  bn::trace_tool::Input input;
  std::string error;
  ASSERT_TRUE(input.Open(path.data(), error)) << error;
  EXPECT_FALSE(input.IsLive());
  EXPECT_FALSE(input.ResetDevice());  // A file has no reset line.
  EXPECT_EQ(ReadBytes(input, 100), std::vector<std::uint8_t>(content.begin(), content.end()));

  std::array<std::uint8_t, 8> buffer{};
  std::size_t count = 0;
  EXPECT_EQ(input.Read(buffer, 10, count), bn::trace_tool::Input::Result::kEnd);
  unlink(path.data());
}

TEST(Input, ReportsAFileItCannotOpen) {
  bn::trace_tool::Input input;
  std::string error;
  EXPECT_FALSE(input.Open("/nonexistent/bn-trace-capture.bin", error));
  EXPECT_NE(error.find("/nonexistent/bn-trace-capture.bin"), std::string::npos);
}

TEST(Input, FindsDevicesByPatternAndChoosesTheOnlyOne) {
  std::array<char, 64> directory{};
  std::snprintf(directory.data(), directory.size(), "/tmp/bn_trace_devices_XXXXXX");
  ASSERT_NE(mkdtemp(directory.data()), nullptr);
  const std::string base = directory.data();
  const auto touch = [&base](const char* name) {
    const std::string path = base + "/" + name;
    const int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    ASSERT_GE(fd, 0);
    close(fd);
  };
  const std::string modemPattern = base + "/cu.usbmodem*";
  const std::string acmPattern = base + "/ttyACM*";
  const std::array<std::string_view, 2> patterns = {modemPattern, acmPattern};
  std::string error;

  EXPECT_TRUE(bn::trace_tool::FindDevices(patterns).empty());
  EXPECT_TRUE(bn::trace_tool::ChooseDevice({}, error).empty());
  EXPECT_NE(error.find("no USB serial device"), std::string::npos);

  touch("cu.usbmodem101");
  touch("cu.Bluetooth-Incoming-Port");
  std::vector<std::string> found = bn::trace_tool::FindDevices(patterns);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(bn::trace_tool::ChooseDevice(found, error), base + "/cu.usbmodem101");

  touch("ttyACM0");
  found = bn::trace_tool::FindDevices(patterns);
  ASSERT_EQ(found.size(), 2u);
  EXPECT_TRUE(bn::trace_tool::ChooseDevice(found, error).empty());
  EXPECT_NE(error.find("more than one"), std::string::npos);
  EXPECT_NE(error.find("cu.usbmodem101"), std::string::npos);
  EXPECT_NE(error.find("ttyACM0"), std::string::npos);

  for (const char* name : {"cu.usbmodem101", "cu.Bluetooth-Incoming-Port", "ttyACM0"}) {
    unlink((base + "/" + name).c_str());
  }
  rmdir(base.c_str());
}

TEST(Input, KnowsWhereUsbSerialDevicesAppear) {
  const auto patterns = bn::trace_tool::SerialDevicePatterns();
  ASSERT_EQ(patterns.size(), 2u);
  EXPECT_EQ(patterns[0], "/dev/cu.usbmodem*");
  EXPECT_EQ(patterns[1], "/dev/ttyACM*");
}

TEST(Input, ReadsASerialDeviceWithoutAlteringAnyByte) {
  // A pseudo-terminal stands in for the USB serial device: this side writes
  // to the master end, the tool opens the other end by name.
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  ASSERT_GE(master, 0);
  ASSERT_EQ(grantpt(master), 0);
  ASSERT_EQ(unlockpt(master), 0);
  const char* deviceName = ptsname(master);
  ASSERT_NE(deviceName, nullptr);

  bn::trace_tool::Input input;
  std::string error;
  ASSERT_TRUE(input.Open(deviceName, error)) << error;
  EXPECT_TRUE(input.IsLive());

  // Every byte value, including the ones a terminal would normally act on:
  // zero, carriage return, line feed, Ctrl-C, Ctrl-D, Ctrl-Q, Ctrl-S, delete.
  std::vector<std::uint8_t> sent(256);
  for (std::size_t i = 0; i < sent.size(); ++i) {
    sent[i] = static_cast<std::uint8_t>(i);
  }
  ASSERT_EQ(write(master, sent.data(), sent.size()), static_cast<ssize_t>(sent.size()));

  EXPECT_EQ(ReadBytes(input, sent.size()), sent);

  std::array<std::uint8_t, 8> buffer{};
  std::size_t count = 0;
  EXPECT_EQ(input.Read(buffer, 20, count), bn::trace_tool::Input::Result::kTimeout);
  close(master);
}

}  // namespace
