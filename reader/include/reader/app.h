#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/byte_sink.h"
#include "core/clock.h"
#include "core/memory_probe.h"
#include "core/trace.h"
#include "core/trace_stream.h"

namespace bn::reader {

// Everything the application needs from the device, as interfaces. Tests
// pass fakes; the real set comes from DefaultServices().
struct Services {
  core::Clock& clock;
  core::TraceSync& traceSync;
  core::ByteSink& serial;
  core::MemoryProbe& memory;
};

// The services of the hardware port this build was linked with.
Services DefaultServices() noexcept;

// Trace records the application's ring holds: 4 KB.
inline constexpr std::size_t kTraceRecordCount = 256;

// The application. So far it owns the diagnostics: the tracer every part
// records into, and the stream that carries trace, log text and metrics to
// the serial line. Reading books comes later.
class App {
 public:
  explicit App(const Services& services) noexcept;
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Installs the tracer and announces the stream. Call once.
  void Start() noexcept;
  bool Started() const noexcept { return started_; }

  // Does the application's periodic work: a heartbeat in the trace every
  // second, the metrics (with fresh heap and stack levels) every five, and
  // moving buffered trace records to the serial line. Call it often; it
  // never waits.
  void Tick() noexcept;

  // Sends whatever trace is still buffered, for an orderly exit.
  void Flush() noexcept;

  // Microseconds since Start(); 0 before it.
  std::uint64_t UptimeUs() const noexcept;

  // For sending log text.
  core::TraceStream& Stream() noexcept { return stream_; }

 private:
  void SampleMemory() noexcept;

  Services services_;
  std::array<core::TraceRecord, kTraceRecordCount> traceStorage_{};
  core::Tracer tracer_;
  core::TraceStream stream_;
  std::uint64_t startedAtUs_ = 0;
  std::uint64_t nextHeartbeatUs_ = 0;
  std::uint64_t nextMetricsUs_ = 0;
  bool started_ = false;
};

// Version of this build, for example "0.1.0-dev+5e068ca".
const char* Version() noexcept;

}  // namespace bn::reader
