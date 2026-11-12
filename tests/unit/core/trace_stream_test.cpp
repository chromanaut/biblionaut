#include "core/trace_stream.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/endian.h"
#include "stream_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::FrameType;
using bn::core::kMaxRecordsPerFrame;
using bn::core::kWireRecordSize;
using bn::core::LoadLe16;
using bn::core::LoadLe32;
using bn::core::MakeSpanId;
using bn::core::TraceKind;
using bn::core::TracePart;
using bn::test::FakeSink;
using bn::test::ParseStream;
using bn::test::StreamItem;

// A tracer, a sink and the stream between them.
template <std::size_t Capacity>
struct StreamFixture {
  bn::test::TraceFixture<Capacity> trace;
  FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};

  std::vector<StreamItem> Items() const { return ParseStream(sink.received); }

  void RecordInstants(std::uint32_t first, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
      trace.tracer.RecordInstant(MakeSpanId(TracePart::kCore, 1), first + i);
    }
  }
};

struct WireRecord {
  std::uint32_t startUs;
  std::uint32_t durationUs;
  std::uint32_t arg;
  std::uint16_t id;
  std::uint8_t task;
  std::uint8_t kind;
};

// The dropped count and records of a trace frame's payload.
struct TraceFrame {
  std::uint32_t dropped = 0;
  std::vector<WireRecord> records;
};

TraceFrame ParseTraceFrame(const StreamItem& item) {
  TraceFrame frame;
  const std::span<const std::uint8_t> payload(item.payload);
  EXPECT_TRUE(item.isFrame);
  EXPECT_EQ(item.type, FrameType::kTrace);
  EXPECT_GE(payload.size(), 4u);
  EXPECT_EQ((payload.size() - 4) % kWireRecordSize, 0u);
  if (payload.size() < 4) {
    return frame;
  }
  frame.dropped = LoadLe32(payload.first<4>());
  for (std::size_t at = 4; at + kWireRecordSize <= payload.size(); at += kWireRecordSize) {
    const auto record = payload.subspan(at, kWireRecordSize);
    frame.records.push_back(WireRecord{.startUs = LoadLe32(record.subspan(0).first<4>()),
                                       .durationUs = LoadLe32(record.subspan(4).first<4>()),
                                       .arg = LoadLe32(record.subspan(8).first<4>()),
                                       .id = LoadLe16(record.subspan(12).first<2>()),
                                       .task = record[14],
                                       .kind = record[15]});
  }
  return frame;
}

// The `arg` of every record in every trace frame, in arrival order.
std::vector<std::uint32_t> AllArgs(const std::vector<StreamItem>& items) {
  std::vector<std::uint32_t> args;
  for (const StreamItem& item : items) {
    for (const WireRecord& record : ParseTraceFrame(item).records) {
      args.push_back(record.arg);
    }
  }
  return args;
}

std::vector<std::uint32_t> Sequence(std::uint32_t first, std::uint32_t count) {
  std::vector<std::uint32_t> values(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values[i] = first + i;
  }
  return values;
}

TEST(TraceStream, HelloCarriesTheProtocolVersionAndFirmwareVersion) {
  StreamFixture<4> fixture;
  EXPECT_TRUE(fixture.stream.SendHello("0.1.0-test"));

  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  ASSERT_TRUE(items[0].isFrame);
  EXPECT_EQ(items[0].type, FrameType::kHello);
  ASSERT_FALSE(items[0].payload.empty());
  EXPECT_EQ(items[0].payload[0], bn::core::kStreamProtocolVersion);
  EXPECT_EQ(items[0].Text(), "0.1.0-test");
}

TEST(TraceStream, SendsALineOfTextWithTheTimeItWasSent) {
  StreamFixture<4> fixture;
  fixture.trace.clock.now = 0x1'2345'6789;
  EXPECT_TRUE(fixture.stream.SendText("page 12 laid out"));

  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].type, FrameType::kText);
  EXPECT_EQ(items[0].TimeUs(), 0x2345'6789u);
  EXPECT_EQ(items[0].Text(), "page 12 laid out");
  EXPECT_EQ(fixture.sink.writes, 1);  // One write per frame, so frames never interleave.
}

TEST(TraceStream, CutsTextThatIsTooLong) {
  StreamFixture<4> fixture;
  const std::string longText(bn::core::kMaxTextLength + 50, 'x');
  EXPECT_TRUE(fixture.stream.SendText(longText));

  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].Text(), std::string(bn::core::kMaxTextLength, 'x'));
}

TEST(TraceStream, SendsMetricsAsOneLine) {
  StreamFixture<4> fixture;
  bn::core::MetricRegistry registry;
  bn::core::Counter reads("sd_reads", registry);
  bn::core::Gauge heap("heap_free", registry);
  reads.Add(3);
  heap.Set(1000);
  fixture.trace.clock.now = 77;

  EXPECT_TRUE(fixture.stream.SendMetrics(registry));
  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].type, FrameType::kMetrics);
  EXPECT_EQ(items[0].TimeUs(), 77u);
  EXPECT_EQ(items[0].Text(), "sd_reads=3 heap_free=1000[1000..1000]");
}

TEST(TraceStream, SendsManyMetricsAsSeveralFramesWithOneTime) {
  StreamFixture<4> fixture;
  bn::core::MetricRegistry registry;
  // Counters cannot be copied or moved, so they are built in place.
  std::array<char, 16> names[24]{};
  std::vector<std::unique_ptr<bn::core::Counter>> counters;
  std::string expected;
  for (int i = 0; i < 24; ++i) {
    std::snprintf(names[i].data(), names[i].size(), "counter_%02d", i);
    counters.push_back(std::make_unique<bn::core::Counter>(names[i].data(), registry));
    counters.back()->Add(static_cast<std::uint32_t>(1000 + i));
    expected += (expected.empty() ? "" : " ") + std::string(names[i].data()) + "=" + std::to_string(1000 + i);
  }
  ASSERT_GT(expected.size(), bn::core::kMaxTextLength);
  fixture.trace.clock.now = 4242;

  EXPECT_TRUE(fixture.stream.SendMetrics(registry));

  const auto items = fixture.Items();
  ASSERT_GT(items.size(), 1u);
  std::string joined;
  for (const StreamItem& item : items) {
    EXPECT_EQ(item.type, FrameType::kMetrics);
    EXPECT_EQ(item.TimeUs(), 4242u);
    EXPECT_LE(item.Text().size(), bn::core::kMaxTextLength);
    joined += (joined.empty() ? "" : " ") + item.Text();
  }
  EXPECT_EQ(joined, expected);
}

TEST(TraceStream, SkipsAMetricThatCouldNeverFitAndSendsTheRest) {
  StreamFixture<4> fixture;
  bn::core::MetricRegistry registry;
  const std::string hugeName(150, 'x');
  bn::core::Counter before("before", registry);
  const bn::core::Counter huge(hugeName.c_str(), registry);
  bn::core::Counter after("after", registry);
  before.Add(1);
  after.Add(2);

  fixture.stream.SendMetrics(registry);

  std::string joined;
  for (const StreamItem& item : fixture.Items()) {
    joined += (joined.empty() ? "" : " ") + item.Text();
  }
  EXPECT_NE(joined.find("before=1"), std::string::npos);
  EXPECT_NE(joined.find("after=2"), std::string::npos);
  EXPECT_EQ(joined.find(hugeName), std::string::npos);
}

TEST(TraceStream, CountsFramesTheSinkRefused) {
  StreamFixture<4> fixture;
  fixture.sink.full = true;
  EXPECT_FALSE(fixture.stream.SendText("lost"));
  EXPECT_FALSE(fixture.stream.SendHello("lost"));
  EXPECT_EQ(fixture.stream.LostFrames(), 2u);
  EXPECT_TRUE(fixture.sink.received.empty());

  fixture.sink.full = false;
  EXPECT_TRUE(fixture.stream.SendText("kept"));
  EXPECT_EQ(fixture.stream.LostFrames(), 2u);
}

TEST(TraceStream, PumpSendsRecordsFieldForField) {
  StreamFixture<8> fixture;
  fixture.trace.sync.task = 2;
  fixture.trace.clock.now = 0x0102'0304;
  const std::uint32_t start = fixture.trace.tracer.Now();
  fixture.trace.clock.now += 0x0A0B;
  fixture.trace.tracer.RecordSpan(MakeSpanId(TracePart::kLayout, 9), start, 0xDEAD'BEEF);
  fixture.trace.tracer.RecordInstant(MakeSpanId(TracePart::kReader, 1), 7);

  EXPECT_EQ(fixture.stream.Pump(), 2u);
  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  const TraceFrame frame = ParseTraceFrame(items[0]);
  EXPECT_EQ(frame.dropped, 0u);
  ASSERT_EQ(frame.records.size(), 2u);

  EXPECT_EQ(frame.records[0].startUs, 0x0102'0304u);
  EXPECT_EQ(frame.records[0].durationUs, 0x0A0Bu);
  EXPECT_EQ(frame.records[0].arg, 0xDEAD'BEEFu);
  EXPECT_EQ(frame.records[0].id, 0x0409);
  EXPECT_EQ(frame.records[0].task, 2);
  EXPECT_EQ(frame.records[0].kind, static_cast<std::uint8_t>(TraceKind::kSpan));

  EXPECT_EQ(frame.records[1].id, 0x0501);
  EXPECT_EQ(frame.records[1].durationUs, 0u);
  EXPECT_EQ(frame.records[1].kind, static_cast<std::uint8_t>(TraceKind::kInstant));
}

TEST(TraceStream, PumpWithNothingToSendWritesNothing) {
  StreamFixture<8> fixture;
  EXPECT_EQ(fixture.stream.Pump(), 0u);
  EXPECT_EQ(fixture.sink.writes, 0);
}

TEST(TraceStream, PumpSplitsRecordsAcrossFrames) {
  StreamFixture<64> fixture;
  fixture.RecordInstants(0, 40);

  EXPECT_EQ(fixture.stream.Pump(), 40u);
  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(ParseTraceFrame(items[0]).records.size(), kMaxRecordsPerFrame);
  EXPECT_EQ(ParseTraceFrame(items[1]).records.size(), kMaxRecordsPerFrame);
  EXPECT_EQ(ParseTraceFrame(items[2]).records.size(), 8u);
  EXPECT_EQ(AllArgs(items), Sequence(0, 40));
}

TEST(TraceStream, PumpSendsALimitedAmountPerCall) {
  StreamFixture<128> fixture;
  fixture.RecordInstants(0, 100);

  const std::size_t first = fixture.stream.Pump();
  EXPECT_GT(first, 0u);
  EXPECT_LT(first, 100u);

  std::size_t total = first;
  for (int calls = 0; calls < 20 && total < 100; ++calls) {
    total += fixture.stream.Pump();
  }
  EXPECT_EQ(total, 100u);
  EXPECT_EQ(AllArgs(fixture.Items()), Sequence(0, 100));
}

TEST(TraceStream, AFullSinkLosesNoRecords) {
  StreamFixture<64> fixture;
  fixture.RecordInstants(0, 20);

  fixture.sink.full = true;
  EXPECT_EQ(fixture.stream.Pump(), 0u);
  EXPECT_EQ(fixture.stream.Pump(), 0u);
  EXPECT_TRUE(fixture.sink.received.empty());

  // More records arrive while the sink is still blocked.
  fixture.RecordInstants(20, 10);
  fixture.sink.full = false;

  std::size_t total = 0;
  for (int calls = 0; calls < 10; ++calls) {
    total += fixture.stream.Pump();
  }
  EXPECT_EQ(total, 30u);
  EXPECT_EQ(AllArgs(fixture.Items()), Sequence(0, 30));
  EXPECT_EQ(fixture.stream.LostFrames(), 0u);
}

TEST(TraceStream, TraceFramesReportRecordsTheRingDropped) {
  StreamFixture<4> fixture;
  fixture.RecordInstants(0, 6);  // The ring holds 4, so the first two are gone.

  EXPECT_EQ(fixture.stream.Pump(), 4u);
  const auto items = fixture.Items();
  ASSERT_EQ(items.size(), 1u);
  const TraceFrame frame = ParseTraceFrame(items[0]);
  EXPECT_EQ(frame.dropped, 2u);
  EXPECT_EQ(AllArgs(items), Sequence(2, 4));
}

}  // namespace
