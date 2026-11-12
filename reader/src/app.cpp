#include "reader/app.h"

#include "core/metrics.h"
#include "hw/platform.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

constexpr std::uint64_t kHeartbeatIntervalUs = 1'000'000;
constexpr std::uint64_t kMetricsIntervalUs = 5'000'000;
// Upper bound on Pump() calls in Flush(), in case the sink never has room.
constexpr int kMaxFlushRounds = 64;

core::Counter appTicks{"app_ticks"};
// Trace records overwritten in the ring before they could be sent.
core::Gauge traceDropped{"trace_dropped"};
// Log and metrics frames the serial line had no room for.
core::Gauge streamLost{"stream_lost"};

}  // namespace

Services DefaultServices() noexcept {
  return Services{.clock = hw::SystemClock(), .traceSync = hw::SystemTraceSync(), .serial = hw::SystemSerial()};
}

App::App(const Services& services) noexcept
    : services_(services),
      tracer_(traceStorage_, services_.clock, services_.traceSync),
      stream_(tracer_, services_.serial) {}

App::~App() {
  if (core::ActiveTracer() == &tracer_) {
    core::SetActiveTracer(nullptr);
  }
}

void App::Start() noexcept {
  core::SetActiveTracer(&tracer_);
  startedAtUs_ = services_.clock.NowUs();
  nextHeartbeatUs_ = startedAtUs_ + kHeartbeatIntervalUs;
  nextMetricsUs_ = startedAtUs_ + kMetricsIntervalUs;
  started_ = true;

  BN_TRACE_TOP_SCOPE(kSpanAppStart);
  stream_.SendHello(Version());
}

void App::Tick() noexcept {
  if (!started_) {
    return;
  }
  appTicks.Add();

  const std::uint64_t now = services_.clock.NowUs();
  if (now >= nextHeartbeatUs_) {
    BN_TRACE_INSTANT(kInstantHeartbeat, static_cast<std::uint32_t>((now - startedAtUs_) / 1'000'000));
    nextHeartbeatUs_ = now + kHeartbeatIntervalUs;
  }
  if (now >= nextMetricsUs_) {
    traceDropped.Set(tracer_.DroppedCount());
    streamLost.Set(stream_.LostFrames());
    stream_.SendMetrics(core::DefaultMetrics());
    nextMetricsUs_ = now + kMetricsIntervalUs;
  }

  stream_.Pump();
}

void App::Flush() noexcept {
  for (int round = 0; round < kMaxFlushRounds; ++round) {
    if (stream_.Pump() == 0) {
      return;
    }
  }
}

std::uint64_t App::UptimeUs() const noexcept { return started_ ? services_.clock.NowUs() - startedAtUs_ : 0; }

const char* Version() noexcept { return BN_VERSION_STRING; }

}  // namespace bn::reader
