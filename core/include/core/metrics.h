#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

// Counters and gauges: running totals and sampled values that describe what
// the system has been doing (SD reads, cache hits, free heap), cheap enough
// to leave on in every build.
//
// A metric is an object with a name. It adds itself to a registry when it is
// constructed, so a part declares the metrics it needs next to the code that
// updates them and nothing else has to know they exist:
//
//   core::Counter sdReads{"sd_reads"};
//   ...
//   sdReads.Add();
//
// The registry can then write every metric as one line of text.

namespace bn::core {

class Counter;
class Gauge;

// The set of metrics to report together. Metrics register while they are
// being constructed, which for the usual namespace-scope metric is before
// main() and on one thread; after that the registry is only read.
class MetricRegistry {
 public:
  constexpr MetricRegistry() noexcept = default;

  MetricRegistry(const MetricRegistry&) = delete;
  MetricRegistry& operator=(const MetricRegistry&) = delete;
  ~MetricRegistry() = default;

  std::size_t CounterCount() const noexcept { return counterCount_; }
  std::size_t GaugeCount() const noexcept { return gaugeCount_; }

  // Writes every metric into `out` as one line, without a newline:
  //
  //   sd_reads=12 sd_bytes=48000 heap_free=91000[76000..130000]
  //
  // Counters come first, then gauges as last[min..max]; each group is in
  // registration order. A gauge that was never set prints as name=-. The
  // text is always terminated. Metrics that do not fit are left out, whole.
  // Returns the length written, not counting the terminator.
  std::size_t FormatLine(std::span<char> out) const noexcept;

 private:
  friend class Counter;
  friend class Gauge;

  Counter* firstCounter_ = nullptr;
  Counter* lastCounter_ = nullptr;
  Gauge* firstGauge_ = nullptr;
  Gauge* lastGauge_ = nullptr;
  std::size_t counterCount_ = 0;
  std::size_t gaugeCount_ = 0;
};

// The registry metrics join unless they are given another one.
MetricRegistry& DefaultMetrics() noexcept;

// A total that only goes up. Safe to update from any task.
class Counter {
 public:
  // `name` must outlive the counter; a string literal.
  explicit Counter(const char* name, MetricRegistry& registry = DefaultMetrics()) noexcept;

  Counter(const Counter&) = delete;
  Counter& operator=(const Counter&) = delete;
  ~Counter() = default;

  void Add(std::uint32_t amount = 1) noexcept { value_.fetch_add(amount, std::memory_order_relaxed); }

  std::uint32_t Value() const noexcept { return value_.load(std::memory_order_relaxed); }
  const char* Name() const noexcept { return name_; }

 private:
  friend class MetricRegistry;

  const char* name_;
  Counter* next_ = nullptr;
  std::atomic<std::uint32_t> value_{0};
};

// A value sampled from time to time, with the lowest and highest seen. Safe
// to update from any task.
class Gauge {
 public:
  // `name` must outlive the gauge; a string literal.
  explicit Gauge(const char* name, MetricRegistry& registry = DefaultMetrics()) noexcept;

  Gauge(const Gauge&) = delete;
  Gauge& operator=(const Gauge&) = delete;
  ~Gauge() = default;

  void Set(std::uint32_t value) noexcept;

  // False until the first Set(); the values below are 0 until then.
  bool HasValue() const noexcept { return hasValue_.load(std::memory_order_relaxed); }
  std::uint32_t Last() const noexcept { return last_.load(std::memory_order_relaxed); }
  std::uint32_t Min() const noexcept { return HasValue() ? min_.load(std::memory_order_relaxed) : 0; }
  std::uint32_t Max() const noexcept { return max_.load(std::memory_order_relaxed); }
  const char* Name() const noexcept { return name_; }

 private:
  friend class MetricRegistry;

  const char* name_;
  Gauge* next_ = nullptr;
  std::atomic<std::uint32_t> last_{0};
  std::atomic<std::uint32_t> min_{UINT32_MAX};
  std::atomic<std::uint32_t> max_{0};
  std::atomic<bool> hasValue_{false};
};

}  // namespace bn::core
