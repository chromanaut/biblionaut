#include "reader/app.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/endian.h"
#include "memory_fakes.h"
#include "reader/trace_ids.h"
#include "stream_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::FrameType;
using bn::test::StreamItem;

struct AppFixture {
  bn::test::FakeClock clock;
  bn::test::FakeSync sync;
  bn::test::FakeSink sink;
  bn::test::FakeMemoryProbe memory;
  bn::reader::App app{bn::reader::Services{.clock = clock, .traceSync = sync, .serial = sink, .memory = memory}};

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

  const auto records = fixture.TraceRecords();
  ASSERT_EQ(records.size(), 4u);
  EXPECT_EQ(records[0].first, kStartId);
  EXPECT_EQ(records[1], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 1}));
  EXPECT_EQ(records[2], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 2}));
  EXPECT_EQ(records[3], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 3}));
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
  // Start, heartbeats 1 to 5, then the memory sample.
  ASSERT_EQ(idsBefore.size(), 7u);
  EXPECT_EQ(idsBefore[5], kHeartbeatId);
  EXPECT_EQ(argsBefore[5], 5u);
  EXPECT_EQ(idsBefore[6], static_cast<std::uint16_t>(bn::reader::kSpanSampleMemory));
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
  ASSERT_EQ(records.size(), 4u);
  EXPECT_EQ(records[0].first, kStartId);
  EXPECT_EQ(records[3], (std::pair<std::uint16_t, std::uint32_t>{kHeartbeatId, 3}));
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
}

TEST(Version, IsNotEmpty) { EXPECT_GT(std::strlen(bn::reader::Version()), 0u); }

}  // namespace
