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
// Pieces of a crash report sent by one Start() or Tick(), so a report does
// not crowd out the trace.
constexpr int kMaxCrashPiecesPerTick = 4;
// Button events taken in by one Tick().
constexpr int kMaxInputEventsPerTick = 16;

core::Counter appTicks{"app_ticks"};
// Trace records overwritten in the ring before they could be sent.
core::Gauge traceDropped{"trace_dropped"};
// Log and metrics frames the serial line had no room for.
core::Gauge streamLost{"stream_lost"};

// The general heap: free now, the largest block that could be allocated now,
// and the least that was ever free. The last is tracked by the system, so it
// catches dips between samples.
core::Gauge heapFree{"heap_free"};
core::Gauge heapLargestBlock{"heap_largest_block"};
core::Gauge heapLowWater{"heap_low_water"};
// Stack the task running the application has never used.
core::Gauge stackHeadroom{"stack_headroom"};
// 1 if the buttons can be read, 0 if not.
core::Gauge inputOk{"input_ok"};

}  // namespace

Services DefaultServices() noexcept {
  return Services{.clock = hw::SystemClock(),
                  .traceSync = hw::SystemTraceSync(),
                  .serial = hw::SystemSerial(),
                  .memory = hw::SystemMemoryProbe(),
                  .crash = hw::SystemCrashStore(),
                  .display = hw::SystemDisplay(),
                  .input = hw::SystemInput()};
}

App::App(const Services& services) noexcept
    : services_(services),
      tracer_(traceStorage_, services_.clock, services_.traceSync),
      stream_(tracer_, services_.serial),
      viewer_(services_.display, services_.clock) {}

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

  {
    BN_TRACE_TOP_SCOPE(kSpanAppStart);
    stream_.SendHello(Version());

    crashReport_ = services_.crash.Saved();
    services_.crash.SetVersion(Version());
    SendCrashReport();
  }
  inputOk.Set(services_.input.Start() ? 1 : 0);
  viewer_.Start();
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

  SendCrashReport();

  core::InputEvent event{};
  for (int taken = 0; taken < kMaxInputEventsPerTick && services_.input.Next(event); ++taken) {
    viewer_.OnInput(event);
  }
  viewer_.Tick();

  const bool metricsDue = now >= nextMetricsUs_;
  if (metricsDue) {
    traceDropped.Set(tracer_.DroppedCount());
    streamLost.Set(stream_.LostFrames());
    SampleMemory();
  }

  // Trace goes out before the metrics, so what reaches the serial line stays
  // in time order: the metrics are stamped now, after every record above.
  stream_.Pump();

  if (metricsDue) {
    stream_.SendMetrics(core::DefaultMetrics());
    nextMetricsUs_ = now + kMetricsIntervalUs;
  }
}

void App::SampleMemory() noexcept {
  // Traced because reading the largest free block walks the heap; how long
  // that takes is worth seeing.
  BN_TRACE_SCOPE(kSpanSampleMemory);
  if (const std::optional<core::HeapStats> heap = services_.memory.Heap()) {
    heapFree.Set(heap->freeBytes);
    heapLargestBlock.Set(heap->largestFreeBlock);
    heapLowWater.Set(heap->lowestFreeBytes);
  }
  if (const std::optional<std::uint32_t> headroom = services_.memory.StackHeadroom()) {
    stackHeadroom.Set(*headroom);
  }
}

void App::SendCrashReport() noexcept {
  for (int pieces = 0; crashReport_ != nullptr && pieces < kMaxCrashPiecesPerTick; ++pieces) {
    const std::size_t sent = stream_.SendCrashPiece(*crashReport_, crashSent_);
    if (sent == 0) {
      // Either the serial line is full, and the next tick tries again, or
      // the whole report has gone.
      if (crashSent_ >= core::EncodedCrashReportSize(*crashReport_)) {
        crashReport_ = nullptr;
      }
      return;
    }
    crashSent_ += sent;
  }
}

void App::Flush() noexcept {
  for (int round = 0; round < kMaxFlushRounds && crashReport_ != nullptr; ++round) {
    SendCrashReport();
  }
  for (int round = 0; round < kMaxFlushRounds; ++round) {
    if (stream_.Pump() == 0) {
      return;
    }
  }
}

std::uint64_t App::UptimeUs() const noexcept { return started_ ? services_.clock.NowUs() - startedAtUs_ : 0; }

const char* Version() noexcept { return BN_VERSION_STRING; }

}  // namespace bn::reader
