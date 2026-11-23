#include "gray_check.h"

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/display.h"
#include "draw/dither.h"
#include "hw/platform.h"

// Only compiled into a build with BN_GRAY_CHECK.

namespace bn::firmware {

namespace {

constexpr std::size_t kWidth = 800;  // The X4's panel; the check says so if it is given another.
constexpr std::size_t kHeight = 480;
constexpr std::size_t kRowBytes = kWidth / 8;
constexpr std::size_t kBandRows = 16;
// Where the picture's thirds begin: the patches, the ramp in four levels,
// the ramp in two.
constexpr std::size_t kFourRampRow = 160;
constexpr std::size_t kTwoRampRow = 320;
constexpr std::uint32_t kLevels = 4;

alignas(4) std::array<std::uint8_t, kBandRows * kRowBytes> band{};
std::array<std::uint8_t, kWidth> row{};
std::array<std::int16_t, draw::Ditherer::ErrorsFor(kWidth)> errors{};

// Lets the other tasks have a turn: the idle task is watched, and the
// firmware is ended if it gets none for a few seconds.
void LetOthersRun() noexcept { vTaskDelay(1); }

// Waits for the panel to be at rest; how long it took, in microseconds.
std::int64_t WaitForPanel(core::Display& display) noexcept {
  const std::int64_t began = esp_timer_get_time();
  while (display.Busy() && esp_timer_get_time() - began < 10000000) {
    LetOthersRun();
  }
  const std::int64_t took = esp_timer_get_time() - began;
  LetOthersRun();
  return took;
}

void Hold(const char* what, unsigned seconds) noexcept {
  esp_rom_printf("gray check: %s; held %u s\n", what, seconds);
  const std::int64_t until = esp_timer_get_time() + (static_cast<std::int64_t>(seconds) * 1000000);
  while (esp_timer_get_time() < until) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Which pixels a plane marks, by their level (0 black to 3 white): a set
// bit for each that `marks` says yes to.
using Marks = bool (*)(unsigned level) noexcept;

bool WhiteOnly(unsigned level) noexcept { return level == 3; }
bool DarkGray(unsigned level) noexcept { return level == 1; }
bool EitherGray(unsigned level) noexcept { return level == 1 || level == 2; }

// The squares of stage 3, 80 pixels a side: each crosses patches and
// ramps alike, so grays left behind show inside the squares.
bool Square(std::size_t y, std::size_t x) noexcept { return ((y / 80) + (x / 80)) % 2 == 0; }

draw::Ditherer ditherer;
bool dithering = false;

// The levels of the picture's row `y`, into `row`; rows are asked for in
// order from the top, so the ramps can be dithered as they go.
void LevelsOf(std::size_t y) noexcept {
  if (y < kFourRampRow) {
    for (std::size_t x = 0; x < kWidth; ++x) {
      row[x] = static_cast<std::uint8_t>(x / (kWidth / kLevels));
    }
    return;
  }
  if (y == kFourRampRow || y == kTwoRampRow) {
    dithering = ditherer.Begin(kWidth, y == kFourRampRow ? kLevels : 2, errors);
  }
  for (std::size_t x = 0; x < kWidth; ++x) {
    row[x] = static_cast<std::uint8_t>((x * 255) / (kWidth - 1));
  }
  if (!dithering || !ditherer.Row(row)) {
    row.fill(0);
    return;
  }
  // Each pixel is now the gray of its level: 0, 85, 170 or 255 in four, 0
  // or 255 in two.
  for (std::uint8_t& pixel : row) {
    pixel = static_cast<std::uint8_t>(pixel / 85);
  }
}

// Writes the whole plane: the picture, marked by `marks`; or the squares.
bool WritePlane(core::Display& display, core::DisplayPlane plane, Marks marks, bool squares) noexcept {
  for (std::size_t first = 0; first < kHeight; first += kBandRows) {
    band.fill(0);
    for (std::size_t r = 0; r < kBandRows; ++r) {
      const std::size_t y = first + r;
      LevelsOf(y);
      for (std::size_t x = 0; x < kWidth; ++x) {
        if (squares ? Square(y, x) : marks(row[x])) {
          band[(r * kRowBytes) + (x / 8)] =
              static_cast<std::uint8_t>(band[(r * kRowBytes) + (x / 8)] | (0x80U >> (x % 8)));
        }
      }
    }
    if (!display.WriteRows(plane, static_cast<std::uint16_t>(first), band)) {
      return false;
    }
  }
  return true;
}

bool Written(bool ok, const char* what, std::int64_t began) noexcept {
  if (ok) {
    esp_rom_printf("gray check: %s written in %u ms\n", what,
                   static_cast<unsigned>((esp_timer_get_time() - began) / 1000));
  } else {
    esp_rom_printf("gray check: %s: the panel would not take it\n", what);
  }
  return ok;
}

bool Refreshed(core::Display& display, bool started, const char* what) noexcept {
  if (!started) {
    esp_rom_printf("gray check: %s: the panel would not start it\n", what);
    return false;
  }
  const std::int64_t took = WaitForPanel(display);
  esp_rom_printf("gray check: %s took %u ms\n", what, static_cast<unsigned>(took / 1000));
  return true;
}

// Stages 1 and 2: the picture in black and white, then the gray pass.
bool ShowInGrays(core::Display& display, core::AfterRefresh afterHalf, const char* how) noexcept {
  esp_rom_printf("gray check: the picture, %s\n", how);
  std::int64_t began = esp_timer_get_time();
  if (!Written(WritePlane(display, core::DisplayPlane::kNew, WhiteOnly, false), "black and white", began) ||
      !Refreshed(display, display.StartRefresh(core::RefreshMode::kHalf, afterHalf), "the half refresh")) {
    return false;
  }
  Hold("in black and white, every gray black", 5);
  began = esp_timer_get_time();
  if (!Written(WritePlane(display, core::DisplayPlane::kNew, DarkGray, false) &&
                   WritePlane(display, core::DisplayPlane::kOld, EitherGray, false),
               "the gray planes", began) ||
      !Refreshed(display, display.StartGrayRefresh(core::AfterRefresh::kPowerOff), "the gray pass")) {
    return false;
  }
  Hold("LOOK NOW: four patches, black to white; the four-level ramp smoother than the two-level one", 30);
  return true;
}

}  // namespace

void RunGrayCheck() noexcept {
  core::Display& display = hw::SystemDisplay();
  if (!display.Start()) {
    esp_rom_printf("gray check: the panel did not start\n");
    return;
  }
  const core::DisplaySize size = display.Size();
  if (size.width != kWidth || size.height != kHeight) {
    esp_rom_printf("gray check: made for a panel of %u by %u, not %u by %u\n", static_cast<unsigned>(kWidth),
                   static_cast<unsigned>(kHeight), static_cast<unsigned>(size.width),
                   static_cast<unsigned>(size.height));
    return;
  }
  if (!Refreshed(display, display.StartRefresh(core::RefreshMode::kFull, core::AfterRefresh::kPowerOff),
                 "a full refresh to white") ||
      !ShowInGrays(display, core::AfterRefresh::kPowerOff, "the drive voltages off between the refreshes")) {
    return;
  }
  std::int64_t began = esp_timer_get_time();
  if (!Written(WritePlane(display, core::DisplayPlane::kNew, nullptr, true), "the squares", began) ||
      !Refreshed(display, display.StartRefresh(core::RefreshMode::kHalf, core::AfterRefresh::kPowerOff),
                 "the half refresh")) {
    return;
  }
  Hold("LOOK NOW: large squares; any trace of the patches or the ramps is grays not cleared", 15);
  if (!ShowInGrays(display, core::AfterRefresh::kStayPowered, "the drive voltages kept up from one to the other")) {
    return;
  }
  // The application begins with a full refresh of its own.
  esp_rom_printf("gray check: done\n");
}

}  // namespace bn::firmware
