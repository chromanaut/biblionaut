#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/byte_sink.h"
#include "core/clock.h"
#include "core/crash.h"
#include "core/display.h"
#include "core/input.h"
#include "core/memory_probe.h"
#include "core/storage.h"
#include "core/trace.h"
#include "core/trace_stream.h"
#include "reader/device_check.h"
#include "reader/pattern_viewer.h"
#include "reader/storage_check.h"

namespace bn::reader {

class BookCheck;

// Everything the application needs from the device, as interfaces. Tests
// pass fakes; the real set comes from DefaultServices().
struct Services {
  core::Clock& clock;
  core::TraceSync& traceSync;
  core::ByteSink& serial;
  core::MemoryProbe& memory;
  core::CrashStore& crash;
  core::Display& display;
  core::Input& input;
  core::Storage& storage;
};

// The services of the hardware port this build was linked with.
Services DefaultServices() noexcept;

// Trace records the application's ring holds: 4 KB.
inline constexpr std::size_t kTraceRecordCount = 256;

// The application. So far it owns the diagnostics: the tracer every part
// records into, the stream that carries trace, log text and metrics to the
// serial line, and passing on the report of a crash that ended an earlier
// run. In place of a book it shows a test pattern that the buttons step
// through (reader/pattern_viewer.h). It gets storage ready, and does
// nothing with it yet. Reading books comes later.
class App {
 public:
  explicit App(const Services& services) noexcept;
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Installs the tracer and announces the stream. If the device kept a
  // report of a crash, starts sending it; Tick() sends the rest. Starts
  // watching the buttons, sets the panel up and puts the first picture of
  // the test pattern on it, which takes some tens of milliseconds, and
  // then gets storage ready while the panel is busy with that. Call once.
  void Start() noexcept;
  bool Started() const noexcept { return started_; }

  // Does the application's periodic work: acting on the buttons, a
  // heartbeat in the trace every second, the metrics (with fresh heap and
  // stack levels) every five, and moving buffered trace records to the
  // serial line. Call it often; it never waits.
  void Tick() noexcept;

  // Sends whatever trace is still buffered, for an orderly exit.
  void Flush() noexcept;

  // Microseconds since Start(); 0 before it.
  std::uint64_t UptimeUs() const noexcept;

  // For sending log text.
  core::TraceStream& Stream() noexcept { return stream_; }

  const PatternViewer& Viewer() const noexcept { return viewer_; }

  // Has the panel's planes read back and checked whenever it comes to rest
  // (PatternViewer::CheckPlanes). Call before Start().
  void CheckPanelPlanes() noexcept { viewer_.CheckPlanes(); }

  // Has storage put through its paces (reader/storage_check.h): once at
  // start, and again whenever the Power button is pressed, which also
  // looks for a card again if none was found before. Call before Start().
  void CheckStorage() noexcept;
  // The check, if CheckStorage() was called; null otherwise.
  const StorageCheck* StorageChecker() const noexcept { return storageCheck_; }

  // Has every book on the card read from end to end
  // (reader/book_check.h): once at start, and again whenever the Power
  // button is pressed, which also looks for a card again if none was found
  // before. Call before Start(). Not together with CheckStorage(): the two
  // would get in each other's way on the card.
  void CheckBooks() noexcept;
  // The check, if CheckBooks() was called; null otherwise.
  const BookCheck* BookChecker() const noexcept { return bookCheck_; }

 private:
  void SampleMemory() noexcept;
  void SendCrashReport() noexcept;
  void StartStorage() noexcept;
  void BeginChecks() noexcept;

  Services services_;
  std::array<core::TraceRecord, kTraceRecordCount> traceStorage_{};
  core::Tracer tracer_;
  core::TraceStream stream_;
  PatternViewer viewer_;
  // The checks, each on the heap and only when asked for: the storage
  // check is 9 KB and the book check 81 KB. They are kept twice. These
  // two are for telling callers about them,
  StorageCheck* storageCheck_ = nullptr;
  BookCheck* bookCheck_ = nullptr;
  // and these are the same two as the application uses them: it reaches
  // a check's code only through DeviceCheck, so that a build which asks
  // for neither has neither in it.
  std::array<DeviceCheck*, 2> checks_{};
  bool storageReady_ = false;
  std::uint64_t startedAtUs_ = 0;
  std::uint64_t nextHeartbeatUs_ = 0;
  std::uint64_t nextMetricsUs_ = 0;
  const core::CrashReport* crashReport_ = nullptr;  // Being sent; null when there is none or it is done.
  std::size_t crashSent_ = 0;                       // Bytes of its encoded form sent so far.
  bool started_ = false;
};

// Version of this build, for example "0.1.0-dev+5e068ca".
const char* Version() noexcept;

}  // namespace bn::reader
