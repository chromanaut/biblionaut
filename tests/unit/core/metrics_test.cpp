#include "core/metrics.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string_view>

namespace {

using bn::core::Counter;
using bn::core::Gauge;
using bn::core::MetricRegistry;

// Registers with the default registry, the way a part's own metrics do.
Counter defaultRegistryProbe{"metrics_test_probe"};

std::string_view Line(const MetricRegistry& registry, std::span<char> buffer) {
  const std::size_t length = registry.FormatLine(buffer);
  EXPECT_EQ(length, std::strlen(buffer.data()));
  return {buffer.data(), length};
}

TEST(Counter, StartsAtZeroAndAdds) {
  MetricRegistry registry;
  Counter reads("reads", registry);
  EXPECT_EQ(reads.Value(), 0u);
  reads.Add();
  reads.Add(41);
  EXPECT_EQ(reads.Value(), 42u);
  EXPECT_STREQ(reads.Name(), "reads");
}

TEST(Gauge, TracksLastLowestAndHighest) {
  MetricRegistry registry;
  Gauge heap("heap_free", registry);
  EXPECT_FALSE(heap.HasValue());
  EXPECT_EQ(heap.Last(), 0u);
  EXPECT_EQ(heap.Min(), 0u);
  EXPECT_EQ(heap.Max(), 0u);

  heap.Set(500);
  heap.Set(200);
  heap.Set(900);
  heap.Set(300);
  EXPECT_TRUE(heap.HasValue());
  EXPECT_EQ(heap.Last(), 300u);
  EXPECT_EQ(heap.Min(), 200u);
  EXPECT_EQ(heap.Max(), 900u);
}

TEST(Gauge, ZeroIsAValue) {
  MetricRegistry registry;
  Gauge depth("depth", registry);
  depth.Set(0);
  EXPECT_TRUE(depth.HasValue());
  EXPECT_EQ(depth.Min(), 0u);
  EXPECT_EQ(depth.Max(), 0u);
}

TEST(MetricRegistry, CountsWhatRegistered) {
  MetricRegistry registry;
  EXPECT_EQ(registry.CounterCount(), 0u);
  EXPECT_EQ(registry.GaugeCount(), 0u);
  const Counter a("a", registry);
  const Counter b("b", registry);
  const Gauge c("c", registry);
  EXPECT_EQ(registry.CounterCount(), 2u);
  EXPECT_EQ(registry.GaugeCount(), 1u);
}

TEST(MetricRegistry, FormatsCountersThenGaugesInRegistrationOrder) {
  MetricRegistry registry;
  Gauge heap("heap_free", registry);
  Counter reads("sd_reads", registry);
  Gauge stack("stack_low", registry);
  Counter bytes("sd_bytes", registry);

  reads.Add(12);
  bytes.Add(48'000);
  heap.Set(130'000);
  heap.Set(76'000);
  heap.Set(91'000);

  std::array<char, 128> buffer{};
  EXPECT_EQ(Line(registry, buffer), "sd_reads=12 sd_bytes=48000 heap_free=91000[76000..130000] stack_low=-");
}

TEST(MetricRegistry, AnEmptyRegistryFormatsAsAnEmptyLine) {
  const MetricRegistry registry;
  std::array<char, 8> buffer{'x', 'x', 'x'};
  EXPECT_EQ(registry.FormatLine(buffer), 0u);
  EXPECT_EQ(buffer[0], '\0');
}

TEST(MetricRegistry, LeavesOutWholeMetricsThatDoNotFit) {
  MetricRegistry registry;
  Counter first("first", registry);
  Counter second("second", registry);
  Counter third("third", registry);
  first.Add(1);
  second.Add(22);
  third.Add(333);

  // "first=1 second=22" is 17 characters; with the terminator it needs 18.
  std::array<char, 18> exact{};
  EXPECT_EQ(Line(registry, exact), "first=1 second=22");

  std::array<char, 17> oneShort{};
  EXPECT_EQ(Line(registry, oneShort), "first=1");

  std::array<char, 4> tiny{};
  EXPECT_EQ(Line(registry, tiny), "");
}

TEST(MetricRegistry, AnEmptyBufferIsLeftAlone) {
  MetricRegistry registry;
  const Counter counter("c", registry);
  EXPECT_EQ(registry.FormatLine({}), 0u);
}

TEST(DefaultMetrics, HoldsMetricsDeclaredAtNamespaceScope) {
  defaultRegistryProbe.Add(3);
  EXPECT_GE(bn::core::DefaultMetrics().CounterCount(), 1u);

  std::array<char, 256> buffer{};
  const std::string_view line = Line(bn::core::DefaultMetrics(), buffer);
  EXPECT_NE(line.find("metrics_test_probe=3"), std::string_view::npos);
}

}  // namespace
