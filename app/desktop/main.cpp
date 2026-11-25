#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include "core/heap.h"
#include "desktop/panel_window.h"
#include "desktop/sim_display.h"
#include "desktop/sim_power.h"
#include "hw/platform.h"
#include "reader/app.h"
#include "reader/drawing_viewer.h"

namespace {

void OnHeapFailure(const bn::core::HeapFailure& failure) noexcept {
  std::fprintf(stderr, "heap allocation failed: %zu bytes, alignment %zu%s\n", failure.size, failure.alignment,
               failure.fatal ? " (fatal)" : "");
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--run-ms N] [--window]\n"
               "  --run-ms N   keep running for N milliseconds (default: start and exit;\n"
               "               with --window, until the window is closed)\n"
               "  --window     show the panel in a window, and take the buttons from the\n"
               "               keyboard as well as from BN_INPUT (a build configured with\n"
               "               -DBN_DESKTOP_WINDOW=ON)\n"
               "Set BN_SERIAL_OUT to a file name to capture the diagnostic stream.\n"
               "Set BN_PLANE_CHECK to have the panel's planes read back and checked\n"
               "whenever it comes to rest.\n"
               "Set BN_STORAGE to a folder to stand for the SD card, and BN_STORAGE_CHECK\n"
               "to have it put through its paces at start and at each (scripted) press\n"
               "of Power.\n"
               "Set BN_BOOK_CHECK instead to have every EPUB in that folder's books folder\n"
               "read from end to end, at the same moments; and BN_BOOK_CHECK_BRIEF with it\n"
               "to have each only opened, and none read through.\n"
               "Set BN_LAYOUT_CHECK instead to have the layout code timed, and the text in\n"
               "that folder's layout folder set in lines, at the same moments.\n"
               "Set BN_DRAWINGS instead to have the SVG, PNG and JPEG files in that\n"
               "folder's drawings folder shown in place of the test pattern, Down and Up\n"
               "going from one to the next; and BN_DRAWING_TOUR as well to have it go\n"
               "round them twice by itself.\n"
               "Set BN_TEXT to a fonts image (made by bn-font) instead to have the text of\n"
               "that folder's layout/paragraphs.txt set in its faces a page at a time:\n"
               "Right and Left turn the page, Confirm changes the size and Back the face.\n",
               program);
}

// The services of one run: `services` with other buttons, and begun for
// another reason.
bn::reader::Services ForRun(const bn::reader::Services& services, bn::core::Input& input,
                            bn::core::StartCause startCause) {
  return bn::reader::Services{.clock = services.clock,
                              .traceSync = services.traceSync,
                              .serial = services.serial,
                              .memory = services.memory,
                              .crash = services.crash,
                              .display = services.display,
                              .input = input,
                              .storage = services.storage,
                              .power = services.power,
                              .startCause = startCause};
}

// What the environment asks of every run.
struct Checks {
  bool planes = false;
  bool storage = false;
  bool books = false;
  bool booksBrief = false;
  bool layout = false;
  bool drawings = false;
  bool drawingTour = false;
  // A fonts image to show text in, if BN_TEXT named one: its bytes.
  std::vector<std::uint8_t> text;
  bool textAsked = false;
};

// How long the application runs: until the window is closed, or until a
// time.
struct Limit {
  bool untilClosed = false;
  std::chrono::steady_clock::time_point deadline;

  bool Over() const { return !untilClosed && std::chrono::steady_clock::now() >= deadline; }
};

// How one run of the application came to an end.
enum class Ended : std::uint8_t {
  kOver,    // The time was up, or the window was closed.
  kAsleep,  // The device was put to sleep.
};

// One run of the application, from its start to its end.
Ended Run(const bn::reader::Services& services, const Checks& checks, bn::hw::PanelWindow* window, const Limit& limit) {
  bn::reader::App app(services);
  if (checks.planes) {
    app.CheckPanelPlanes();
  }
  if (checks.storage) {
    app.CheckStorage();
  }
  if (checks.books) {
    app.CheckBooks(checks.booksBrief);
  }
  if (checks.layout) {
    app.CheckLayout();
  }
  if (checks.drawings) {
    app.ViewDrawings(bn::reader::DrawingViewer::kFolder, checks.drawingTour);
  }
  if (checks.textAsked) {
    app.ViewText(checks.text);
  }
  app.Start();
  while (!limit.Over()) {
    app.Tick();
    if (app.Asleep()) {
      return Ended::kAsleep;  // It has sent everything already.
    }
    if (window != nullptr && !window->Pump()) {
      break;
    }
    // As the device: a pause before each turn, none while there is work
    // waiting but for one now and then.
    const std::uint64_t pauseUs = app.PauseUs();
    if (pauseUs > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(pauseUs));
    }
  }
  app.Tick();
  app.Flush();
  return app.Asleep() ? Ended::kAsleep : Ended::kOver;
}

// With the device asleep and no application running, waits for Power to
// be pressed, passing over every other button. False if the time is up, or
// the window is closed, first.
bool WaitForPower(bn::core::Input& input, bn::hw::PanelWindow* window, const Limit& limit) {
  while (!limit.Over()) {
    if (window != nullptr && !window->Pump()) {
      return false;
    }
    bn::core::InputEvent event{};
    while (input.Next(event)) {
      if (event.kind == bn::core::InputEventKind::kPress && event.button == bn::core::Button::kPower) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  long runMs = 0;
  bool runMsGiven = false;
  bool withWindow = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--run-ms") == 0 && i + 1 < argc) {
      char* end = nullptr;
      runMs = std::strtol(argv[++i], &end, 10);
      runMsGiven = true;
      if (end == nullptr || *end != '\0' || runMs < 0) {
        PrintUsage(argv[0]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--window") == 0) {
      withWindow = true;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  bn::core::SetHeapFailureHandler(OnHeapFailure);

  // NOLINTBEGIN(concurrency-mt-unsafe): read once, before any other thread exists.
  Checks checks{.planes = std::getenv("BN_PLANE_CHECK") != nullptr,
                .storage = std::getenv("BN_STORAGE_CHECK") != nullptr,
                .books = std::getenv("BN_BOOK_CHECK") != nullptr,
                .booksBrief = std::getenv("BN_BOOK_CHECK_BRIEF") != nullptr,
                .layout = std::getenv("BN_LAYOUT_CHECK") != nullptr,
                .drawings = std::getenv("BN_DRAWINGS") != nullptr,
                .drawingTour = std::getenv("BN_DRAWING_TOUR") != nullptr,
                .text = {},
                .textAsked = false};
  const char* textImage = std::getenv("BN_TEXT");
  // NOLINTEND(concurrency-mt-unsafe)
  if (textImage != nullptr) {
    std::ifstream file(textImage, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "BN_TEXT: cannot read %s\n", textImage);
      return 2;
    }
    checks.text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    checks.textAsked = true;
  }
  if (static_cast<int>(checks.storage) + static_cast<int>(checks.books) + static_cast<int>(checks.layout) +
          static_cast<int>(checks.drawings) + static_cast<int>(checks.textAsked) >
      1) {
    std::fprintf(stderr,
                 "BN_STORAGE_CHECK, BN_BOOK_CHECK, BN_LAYOUT_CHECK, BN_DRAWINGS and BN_TEXT would get in each other's "
                 "way: set one of them.\n");
    return 2;
  }

  // The window is opened first, so that the application is given its
  // buttons. It shows the same simulated panel as there is without it.
  std::unique_ptr<bn::hw::PanelWindow> window;
  if (withWindow) {
    window = bn::hw::PanelWindow::Open(bn::hw::SystemClock(), bn::hw::SystemSimDisplay(), bn::hw::SystemSimPower(),
                                       bn::hw::SystemInput());
    if (window == nullptr) {
      return 1;
    }
  }
  const bn::reader::Services defaults = bn::reader::DefaultServices();
  bn::core::Input& buttons = window != nullptr ? window->Buttons() : defaults.input;
  std::printf("Biblionaut %s (%s)\n", bn::reader::Version(), bn::hw::PortName());
  if (window != nullptr) {
    std::printf("%s", bn::hw::PanelWindow::KeysHelp());
  }
  std::fflush(stdout);

  const Limit limit{.untilClosed = window != nullptr && !runMsGiven,
                    .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(runMs)};
  // Each sleep ends a run, as it does on a device, and waking begins the
  // next. What a device would lose in its sleep, the desktop keeps: the
  // panel's simulation, the scripted presses still to come, the counters.
  bn::core::StartCause cause = defaults.startCause;
  while (Run(ForRun(defaults, buttons, cause), checks, window.get(), limit) == Ended::kAsleep) {
    std::printf("Asleep: Power wakes it.\n");
    std::fflush(stdout);
    if (!WaitForPower(buttons, window.get(), limit)) {
      break;
    }
    bn::hw::SystemSimPower().Wake();
    cause = bn::core::StartCause::kDeepSleep;
  }
  return 0;
}
