#include "window_check.h"

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/display.h"
#include "hw/platform.h"

// Only compiled into a build with BN_WINDOW_CHECK.

namespace bn::firmware {

namespace {

// Rows read back, or written as whole rows, at a time.
constexpr std::size_t kBandRows = 80;
// The odd windows, written over a plane already filled: each corner, one
// across the middle of every row, one down the whole height, and a few in
// between.
constexpr std::array<core::DisplayWindow, 8> kOddWindows = {{
    {.firstRow = 0, .rows = 1, .firstByte = 0, .bytes = 1},
    {.firstRow = 0, .rows = 1, .firstByte = 99, .bytes = 1},
    {.firstRow = 479, .rows = 1, .firstByte = 0, .bytes = 1},
    {.firstRow = 479, .rows = 1, .firstByte = 99, .bytes = 1},
    {.firstRow = 101, .rows = 37, .firstByte = 13, .bytes = 3},
    {.firstRow = 200, .rows = 80, .firstByte = 50, .bytes = 50},
    {.firstRow = 7, .rows = 9, .firstByte = 98, .bytes = 2},
    {.firstRow = 0, .rows = 480, .firstByte = 57, .bytes = 1},
}};

alignas(4) std::array<std::uint8_t, kBandRows * 100> buffer{};

// The byte a pattern has at a place: every byte of a plane different from
// its neighbours, and no two patterns alike.
std::uint8_t PatternAt(std::uint32_t seed, std::size_t row, std::size_t byte) noexcept {
  std::uint32_t x = (static_cast<std::uint32_t>(row) * 131U) + (static_cast<std::uint32_t>(byte) * 7919U) + seed;
  x ^= x >> 7U;
  x *= 0x2545F491U;
  x ^= x >> 13U;
  return static_cast<std::uint8_t>(x >> 24U);
}

bool Inside(const core::DisplayWindow& window, std::size_t row, std::size_t byte) noexcept {
  return row >= window.firstRow && row < std::size_t{window.firstRow} + window.rows && byte >= window.firstByte &&
         byte < std::size_t{window.firstByte} + window.bytes;
}

const char* PlaneName(core::DisplayPlane plane) noexcept { return plane == core::DisplayPlane::kNew ? "new" : "old"; }

// Lets the other tasks have a turn: the idle task is watched, and the
// firmware is ended if it gets none for a few seconds.
void LetOthersRun() noexcept { vTaskDelay(1); }

void WaitForPanel(core::Display& display) noexcept {
  const std::int64_t began = esp_timer_get_time();
  while (display.Busy() && esp_timer_get_time() - began < 10000000) {
    LetOthersRun();
  }
  LetOthersRun();
}

// Fills `window` of a plane with `seed`'s pattern. False if the panel would
// not take it.
bool WriteWindow(core::Display& display, core::DisplayPlane plane, core::DisplayWindow window,
                 std::uint32_t seed) noexcept {
  const std::span<std::uint8_t> data = std::span(buffer).first(window.Size());
  for (std::size_t row = 0; row < window.rows; ++row) {
    for (std::size_t byte = 0; byte < window.bytes; ++byte) {
      data[(row * window.bytes) + byte] = PatternAt(seed, window.firstRow + row, window.firstByte + byte);
    }
  }
  return display.WriteWindow(plane, window, data);
}

// Fills a whole plane with `seed`'s pattern by whole rows, a band at a time.
bool FillByRows(core::Display& display, core::DisplayPlane plane, std::uint32_t seed) noexcept {
  const core::DisplaySize size = display.Size();
  const std::size_t rowBytes = size.RowBytes();
  for (std::size_t first = 0; first < size.height; first += kBandRows) {
    const std::span<std::uint8_t> band = std::span(buffer).first(kBandRows * rowBytes);
    for (std::size_t row = 0; row < kBandRows; ++row) {
      for (std::size_t byte = 0; byte < rowBytes; ++byte) {
        band[(row * rowBytes) + byte] = PatternAt(seed, first + row, byte);
      }
    }
    if (!display.WriteRows(plane, static_cast<std::uint16_t>(first), band)) {
      return false;
    }
  }
  return true;
}

// Fills a whole plane with `seed`'s pattern by windows one byte wide and
// the panel's height.
bool FillByColumns(core::Display& display, core::DisplayPlane plane, std::uint32_t seed) noexcept {
  const core::DisplaySize size = display.Size();
  for (std::size_t byte = 0; byte < size.RowBytes(); ++byte) {
    if (!WriteWindow(display, plane,
                     {.firstRow = 0, .rows = size.height, .firstByte = static_cast<std::uint16_t>(byte), .bytes = 1},
                     seed)) {
      return false;
    }
  }
  return true;
}

// Fills a whole plane with `seed`'s pattern by windows of eight rows the
// panel's width.
bool FillByEights(core::Display& display, core::DisplayPlane plane, std::uint32_t seed) noexcept {
  const core::DisplaySize size = display.Size();
  for (std::size_t row = 0; row < size.height; row += 8) {
    if (!WriteWindow(display, plane,
                     {.firstRow = static_cast<std::uint16_t>(row),
                      .rows = 8,
                      .firstByte = 0,
                      .bytes = static_cast<std::uint16_t>(size.RowBytes())},
                     seed)) {
      return false;
    }
  }
  return true;
}

// Reads a plane back and says on the console whether it holds `seed`'s
// pattern, with `oddSeed`'s in the odd windows if `odd`.
void Check(core::Display& display, core::DisplayPlane plane, const char* how, std::uint32_t seed, bool odd,
           std::uint32_t oddSeed, std::int64_t writeUs) noexcept {
  const core::DisplaySize size = display.Size();
  const std::size_t rowBytes = size.RowBytes();
  std::size_t wrong = 0;
  std::size_t firstRow = 0;
  std::size_t firstByte = 0;
  unsigned got = 0;
  unsigned wanted = 0;
  bool read = true;
  for (std::size_t first = 0; read && first < size.height; first += kBandRows) {
    const std::span<std::uint8_t> band = std::span(buffer).first(kBandRows * rowBytes);
    LetOthersRun();
    read = display.ReadRows(plane, static_cast<std::uint16_t>(first), band);
    for (std::size_t row = 0; read && row < kBandRows; ++row) {
      for (std::size_t byte = 0; byte < rowBytes; ++byte) {
        bool inOdd = false;
        for (const core::DisplayWindow& window : kOddWindows) {
          inOdd = inOdd || (odd && Inside(window, first + row, byte));
        }
        const std::uint8_t expected = PatternAt(inOdd ? oddSeed : seed, first + row, byte);
        const std::uint8_t found = band[(row * rowBytes) + byte];
        if (found != expected) {
          if (wrong == 0) {
            firstRow = first + row;
            firstByte = byte;
            got = found;
            wanted = expected;
          }
          ++wrong;
        }
      }
    }
  }
  if (!read) {
    esp_rom_printf("window check: %s plane, %s: the plane could not be read back\n", PlaneName(plane), how);
    return;
  }
  if (wrong == 0) {
    esp_rom_printf("window check: %s plane, %s: right, written in %u us\n", PlaneName(plane), how,
                   static_cast<unsigned>(writeUs));
  } else {
    esp_rom_printf(
        "window check: %s plane, %s: WRONG in %u bytes, the first at row %u byte %u (0x%02x, not 0x%02x); "
        "written in %u us\n",
        PlaneName(plane), how, static_cast<unsigned>(wrong), static_cast<unsigned>(firstRow),
        static_cast<unsigned>(firstByte), got, wanted, static_cast<unsigned>(writeUs));
  }
}

using Fill = bool (*)(core::Display&, core::DisplayPlane, std::uint32_t) noexcept;

void Try(core::Display& display, core::DisplayPlane plane, const char* how, Fill fill, std::uint32_t seed) noexcept {
  WaitForPanel(display);
  const std::int64_t began = esp_timer_get_time();
  const bool written = fill(display, plane, seed);
  const std::int64_t took = esp_timer_get_time() - began;
  if (!written) {
    esp_rom_printf("window check: %s plane, %s: the panel would not take it\n", PlaneName(plane), how);
    return;
  }
  Check(display, plane, how, seed, false, 0, took);
}

}  // namespace

void RunWindowCheck() noexcept {
  core::Display& display = hw::SystemDisplay();
  if (!display.Start()) {
    esp_rom_printf("window check: the panel did not start\n");
    return;
  }
  const core::DisplaySize size = display.Size();
  esp_rom_printf("window check: a panel of %u by %u, %u bytes a row\n", static_cast<unsigned>(size.width),
                 static_cast<unsigned>(size.height), static_cast<unsigned>(size.RowBytes()));
  std::uint32_t seed = 1;
  for (const core::DisplayPlane plane : {core::DisplayPlane::kNew, core::DisplayPlane::kOld}) {
    // Each way twice, with another pattern, so that what one left behind
    // cannot pass for what the next wrote.
    for (int round = 0; round < 2; ++round) {
      Try(display, plane, "whole rows", FillByRows, seed++);
      Try(display, plane, "columns of a byte", FillByColumns, seed++);
      Try(display, plane, "eight rows at a time", FillByEights, seed++);
    }
    // The odd windows, over a plane filled by whole rows.
    WaitForPanel(display);
    const std::uint32_t base = seed++;
    const std::uint32_t over = seed++;
    if (!FillByRows(display, plane, base)) {
      esp_rom_printf("window check: %s plane: the panel would not take whole rows\n", PlaneName(plane));
      continue;
    }
    const std::int64_t began = esp_timer_get_time();
    bool written = true;
    for (const core::DisplayWindow& window : kOddWindows) {
      written = written && WriteWindow(display, plane, window, over);
    }
    const std::int64_t took = esp_timer_get_time() - began;
    if (!written) {
      esp_rom_printf("window check: %s plane, odd windows: the panel would not take them\n", PlaneName(plane));
      continue;
    }
    Check(display, plane, "odd windows over whole rows", base, true, over, took);
  }
  // What a window refuses.
  const std::array<std::uint8_t, 2> two{};
  const bool refused =
      !display.WriteWindow(core::DisplayPlane::kNew, {.firstRow = 0, .rows = 1, .firstByte = 100, .bytes = 1},
                           std::span(two).first(1)) &&
      !display.WriteWindow(core::DisplayPlane::kNew, {.firstRow = 480, .rows = 1, .firstByte = 0, .bytes = 1},
                           std::span(two).first(1)) &&
      !display.WriteWindow(core::DisplayPlane::kNew, {.firstRow = 0, .rows = 1, .firstByte = 0, .bytes = 1}, two);
  esp_rom_printf("window check: windows outside the panel, or not filled exactly, %s\n",
                 refused ? "refused" : "NOT REFUSED");
  esp_rom_printf("window check: done\n");
}

}  // namespace bn::firmware
