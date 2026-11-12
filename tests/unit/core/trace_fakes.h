#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/clock.h"
#include "core/trace.h"

namespace bn::test {

class FakeClock final : public core::Clock {
 public:
  std::uint64_t NowUs() noexcept override { return now; }
  std::uint64_t now = 0;
};

// Counts locking so tests can check that every ring access is guarded and
// that locks are released.
class FakeSync final : public core::TraceSync {
 public:
  void Lock() noexcept override {
    ++lockCount;
    ++depth;
    if (depth > maxDepth) {
      maxDepth = depth;
    }
  }
  void Unlock() noexcept override { --depth; }
  std::uint8_t CurrentTask() noexcept override { return task; }

  int lockCount = 0;
  int depth = 0;
  int maxDepth = 0;
  std::uint8_t task = 0;
};

// A tracer over its own storage, installed as the active tracer for the
// lifetime of the object.
template <std::size_t Capacity>
struct TraceFixture {
  TraceFixture() noexcept { core::SetActiveTracer(&tracer); }
  ~TraceFixture() { core::SetActiveTracer(nullptr); }

  TraceFixture(const TraceFixture&) = delete;
  TraceFixture& operator=(const TraceFixture&) = delete;

  FakeClock clock;
  FakeSync sync;
  std::array<core::TraceRecord, Capacity> storage{};
  core::Tracer tracer{storage, clock, sync};
};

inline constexpr core::SpanId kSpanOuter = core::MakeSpanId(core::TracePart::kCore, 1);
inline constexpr core::SpanId kSpanInner = core::MakeSpanId(core::TracePart::kCore, 2);
inline constexpr core::SpanId kSpanMark = core::MakeSpanId(core::TracePart::kReader, 7);

}  // namespace bn::test
