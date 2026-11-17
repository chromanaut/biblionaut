#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/byte_sink.h"
#include "core/clock.h"
#include "core/crash.h"
#include "core/display.h"
#include "core/input.h"
#include "core/memory_probe.h"
#include "core/power.h"
#include "core/start.h"
#include "core/storage.h"
#include "core/trace.h"
#include "core/trace_stream.h"
#include "reader/device_check.h"
#include "reader/pattern_viewer.h"
#include "reader/storage_check.h"

namespace bn::reader {

class BookCheck;
class DrawingViewer;
class LayoutCheck;
class Screen;

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
  core::Power& power;
  // Why this run began, to be told at the head of the stream.
  core::StartCause startCause = core::StartCause::kUnknown;
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

  // How long the loop should pause before the next Tick(): kTurnPauseUs,
  // which is how long a button press can wait before it is acted on; or
  // nothing while there is work waiting that can be done now, so that a
  // drawing is not drawn a pause at a time. Even then, a pause at least
  // every kLongestRunUs, for whatever runs only when the application waits:
  // on the device, the idle task, which the watchdog watches.
  static constexpr std::uint64_t kTurnPauseUs = 10'000;
  static constexpr std::uint64_t kLongestRunUs = 250'000;
  std::uint64_t PauseUs() noexcept;
  // Whether there is work waiting that can be done now (Screen::WorkWaiting()).
  bool WorkWaiting() const noexcept;

  // Sends whatever trace is still buffered, for an orderly exit.
  void Flush() noexcept;

  // Power puts the device to sleep, where it can sleep (core/power.h) and
  // no device check has the button. Once the panel is at rest, with its
  // voltages off and its controller asleep, the trace is sent and the
  // device put to sleep, where a device stays until it starts again. Where
  // that comes back (on the desktop), this says so, and Tick() does nothing
  // more: the run is over.
  bool Asleep() const noexcept { return asleep_; }

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
  // would get in each other's way on the card. With `brief`, each book is
  // opened and none is read through.
  void CheckBooks(bool brief = false) noexcept;
  // The check, if CheckBooks() was called; null otherwise.
  const BookCheck* BookChecker() const noexcept { return bookCheck_; }

  // Has the layout code timed (reader/layout_check.h), and the text on the
  // card set: once at start, and again whenever the Power button is
  // pressed, as the book check is. Call before Start(). Not together with
  // CheckStorage() or CheckBooks(), which would take the card's time while
  // this is being timed.
  void CheckLayout() noexcept;
  // The check, if CheckLayout() was called; null otherwise.
  const LayoutCheck* LayoutChecker() const noexcept { return layoutCheck_; }

  // Shows the drawings in `folder` on storage, one at a time, in place of
  // the test pattern (reader/drawing_viewer.h). Storage is then got ready
  // before the panel is, and the first drawing comes up once it is. Call
  // before Start(). Not together with the checks, which would take the
  // card's time while drawings are being read from it.
  // With `tour`, the viewer goes round the drawings once by itself
  // (DrawingViewer::TourOnce()).
  void ViewDrawings(std::string_view folder, bool tour = false) noexcept;
  // The viewer, if ViewDrawings() was called; null otherwise.
  const DrawingViewer* DrawingsViewer() const noexcept { return drawingViewer_; }

 private:
  void SampleMemory() noexcept;
  void SendCrashReport() noexcept;
  void StartStorage() noexcept;
  void BeginChecks() noexcept;
  void OnPower(std::uint64_t pressedAtUs) noexcept;
  void GoToSleep() noexcept;

  Services services_;
  std::array<core::TraceRecord, kTraceRecordCount> traceStorage_{};
  core::Tracer tracer_;
  core::TraceStream stream_;
  PatternViewer viewer_;
  // The checks, each on the heap and only when asked for: the storage
  // check is 9 KB, the book check 109 KB and the layout check 93 KB. They
  // are kept twice. These are for telling callers about them,
  StorageCheck* storageCheck_ = nullptr;
  BookCheck* bookCheck_ = nullptr;
  LayoutCheck* layoutCheck_ = nullptr;
  // and these are the same as the application uses them: it reaches a
  // check's code only through DeviceCheck, so that a build which asks for
  // none has none in it.
  std::array<DeviceCheck*, 3> checks_{};
  // In place of the test pattern, if asked for: the drawing viewer, kept
  // twice for the same reason as the checks, and 94 KB of its own memory.
  DrawingViewer* drawingViewer_ = nullptr;
  Screen* screen_ = nullptr;
  bool storageReady_ = false;
  std::uint64_t startedAtUs_ = 0;
  std::uint64_t nextHeartbeatUs_ = 0;
  std::uint64_t nextMetricsUs_ = 0;
  std::uint64_t lastPauseUs_ = 0;                   // When PauseUs() last asked for a pause.
  const core::CrashReport* crashReport_ = nullptr;  // Being sent; null when there is none or it is done.
  std::size_t crashSent_ = 0;                       // Bytes of its encoded form sent so far.
  bool started_ = false;
  // Power was pressed to sleep, at this time; and the device was put to sleep.
  bool sleepAsked_ = false;
  std::uint64_t sleepPressedAtUs_ = 0;
  bool asleep_ = false;
};

// Version of this build, for example "0.1.0-dev+5e068ca".
const char* Version() noexcept;

}  // namespace bn::reader
