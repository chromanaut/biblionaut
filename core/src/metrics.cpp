#include "core/metrics.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace bn::core {

namespace {

// Constant-initialised, so it exists before any metric in another file is
// constructed, whatever order the files' static objects come up in.
constinit MetricRegistry defaultRegistry;

// Longest single metric the line can carry, including its name.
constexpr std::size_t kMaxItemLength = 96;

// Appends `item` to the line if it fits whole, with a separating space after
// the first item. Returns false, leaving the line as it was, if it does not.
bool AppendItem(std::span<char> out, std::size_t& length, const char* item, int itemLength) noexcept {
  if (itemLength < 0 || static_cast<std::size_t>(itemLength) >= kMaxItemLength) {
    return false;
  }
  const auto size = static_cast<std::size_t>(itemLength);
  const std::size_t separator = length > 0 ? 1 : 0;
  if (length + separator + size >= out.size()) {
    return false;
  }
  if (separator != 0) {
    out[length++] = ' ';
  }
  std::memcpy(out.data() + length, item, size);
  length += size;
  out[length] = '\0';
  return true;
}

}  // namespace

MetricRegistry& DefaultMetrics() noexcept { return defaultRegistry; }

Counter::Counter(const char* name, MetricRegistry& registry) noexcept : name_(name) {
  if (registry.lastCounter_ != nullptr) {
    registry.lastCounter_->next_ = this;
  } else {
    registry.firstCounter_ = this;
  }
  registry.lastCounter_ = this;
  ++registry.counterCount_;
}

Gauge::Gauge(const char* name, MetricRegistry& registry) noexcept : name_(name) {
  if (registry.lastGauge_ != nullptr) {
    registry.lastGauge_->next_ = this;
  } else {
    registry.firstGauge_ = this;
  }
  registry.lastGauge_ = this;
  ++registry.gaugeCount_;
}

void Gauge::Set(std::uint32_t value) noexcept {
  last_.store(value, std::memory_order_relaxed);

  std::uint32_t seen = min_.load(std::memory_order_relaxed);
  while (value < seen && !min_.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
  seen = max_.load(std::memory_order_relaxed);
  while (value > seen && !max_.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }

  hasValue_.store(true, std::memory_order_relaxed);
}

std::size_t MetricRegistry::FormatLine(std::span<char> out) const noexcept { return FormatItems(out, 0).length; }

MetricRegistry::Formatted MetricRegistry::FormatItems(std::span<char> out, std::size_t first) const noexcept {
  Formatted result;
  if (out.empty()) {
    return result;
  }
  out[0] = '\0';

  char item[kMaxItemLength];
  std::size_t index = 0;

  for (const Counter* counter = firstCounter_; counter != nullptr; counter = counter->next_) {
    if (index++ < first) {
      continue;
    }
    const int itemLength = std::snprintf(item, sizeof(item), "%s=%" PRIu32, counter->Name(), counter->Value());
    if (!AppendItem(out, result.length, item, itemLength)) {
      return result;
    }
    ++result.count;
  }
  for (const Gauge* gauge = firstGauge_; gauge != nullptr; gauge = gauge->next_) {
    if (index++ < first) {
      continue;
    }
    const int itemLength = gauge->HasValue()
                               ? std::snprintf(item, sizeof(item), "%s=%" PRIu32 "[%" PRIu32 "..%" PRIu32 "]",
                                               gauge->Name(), gauge->Last(), gauge->Min(), gauge->Max())
                               : std::snprintf(item, sizeof(item), "%s=-", gauge->Name());
    if (!AppendItem(out, result.length, item, itemLength)) {
      return result;
    }
    ++result.count;
  }
  return result;
}

}  // namespace bn::core
