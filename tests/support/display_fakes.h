#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "core/display.h"

namespace bn::test {

// A panel that records what is asked of it. It is absent (Start() fails)
// unless the test says otherwise, and busy only when the test says so.
//
// It keeps the rules core/display.h gives for the two planes: a fast
// refresh leaves alone the pixels where both say white, and afterwards the
// two have changed places; switching the drive voltages off puts them back
// in their first places. So `shown` is what a real panel would show, and
// a test of code that drives a panel can simply ask whether that is right.
class FakeDisplay final : public core::Display {
 public:
  enum class Op : std::uint8_t { kStart, kWrite, kRefresh, kPowerOff, kSleep };

  struct Call {
    Op op;
    core::DisplayPlane plane = core::DisplayPlane::kNew;          // kWrite.
    std::uint16_t firstRow = 0;                                   // kWrite.
    std::size_t rowCount = 0;                                     // kWrite.
    core::RefreshMode mode = core::RefreshMode::kFull;            // kRefresh.
    core::AfterRefresh after = core::AfterRefresh::kStayPowered;  // kRefresh.
  };

  FakeDisplay() { Resize(size); }

  // Makes it a panel of another shape, blank. For before it is started.
  void Resize(core::DisplaySize newSize) {
    size = newSize;
    const std::size_t bytes = size.RowBytes() * size.height;
    memory[0].assign(bytes, 0xFF);
    memory[1].assign(bytes, 0xFF);
    shown.assign(bytes, 0xFF);
    newIs = 0;
  }

  core::DisplaySize Size() const noexcept override { return size; }

  [[nodiscard]] bool Start() noexcept override {
    calls.push_back({.op = Op::kStart});
    started = present;
    busy = false;  // Setting a panel up stops whatever it was doing.
    powered = false;
    if (present) {
      // Both planes blank, and in their first places.
      memory[0].assign(memory[0].size(), 0xFF);
      memory[1].assign(memory[1].size(), 0xFF);
      newIs = 0;
    }
    return present;
  }

  [[nodiscard]] bool WriteRows(core::DisplayPlane plane, std::uint16_t firstRow,
                               std::span<const std::uint8_t> rows) noexcept override {
    const std::size_t rowBytes = size.RowBytes();
    if (!started || busy || failWrites || rows.empty() || rows.size() % rowBytes != 0 ||
        firstRow + (rows.size() / rowBytes) > size.height) {
      ++refused;
      return false;
    }
    calls.push_back({.op = Op::kWrite, .plane = plane, .firstRow = firstRow, .rowCount = rows.size() / rowBytes});
    std::memcpy(Plane(plane).data() + (firstRow * rowBytes), rows.data(), rows.size());
    return true;
  }

  // Not recorded in `calls`: reading changes nothing.
  [[nodiscard]] bool ReadRows(core::DisplayPlane plane, std::uint16_t firstRow,
                              std::span<std::uint8_t> rows) noexcept override {
    const std::size_t rowBytes = size.RowBytes();
    if (!started || busy || !readable || rows.empty() || rows.size() % rowBytes != 0 ||
        firstRow + (rows.size() / rowBytes) > size.height) {
      return false;
    }
    ++reads;
    std::memcpy(rows.data(), Plane(plane).data() + (firstRow * rowBytes), rows.size());
    return true;
  }

  [[nodiscard]] bool StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept override {
    if (!started || busy) {
      ++refused;
      return false;
    }
    calls.push_back({.op = Op::kRefresh, .mode = mode, .after = after});
    const std::vector<std::uint8_t>& newPlane = Plane(core::DisplayPlane::kNew);
    const std::vector<std::uint8_t>& oldPlane = Plane(core::DisplayPlane::kOld);
    oldAtRefresh.push_back(oldPlane);  // As it was when asked.
    if (mode == core::RefreshMode::kFast) {
      for (std::size_t i = 0; i < shown.size(); ++i) {
        const unsigned leftAlone = static_cast<unsigned>(newPlane[i]) & oldPlane[i];  // Both white.
        shown[i] = static_cast<std::uint8_t>((shown[i] & leftAlone) | (newPlane[i] & ~leftAlone));
      }
      newIs = 1 - newIs;
    } else {
      shown = newPlane;
    }
    powered = after == core::AfterRefresh::kStayPowered;
    if (!powered) {
      newIs = 0;
    }
    busy = true;
    return true;
  }

  bool Busy() noexcept override { return busy; }

  [[nodiscard]] bool PowerOff() noexcept override {
    if (!started || busy) {
      ++refused;
      return false;
    }
    calls.push_back({.op = Op::kPowerOff});
    busy = powered;  // Switching off takes a moment; with nothing to switch off, none.
    if (powered) {
      newIs = 0;
    }
    powered = false;
    return true;
  }

  [[nodiscard]] bool Sleep() noexcept override {
    if (busy) {
      ++refused;
      return false;
    }
    calls.push_back({.op = Op::kSleep});
    started = false;
    powered = false;
    return true;
  }

  // What the plane now in that role holds.
  std::vector<std::uint8_t>& Plane(core::DisplayPlane plane) noexcept {
    return memory[plane == core::DisplayPlane::kNew ? newIs : 1 - newIs];
  }

  std::size_t Count(Op op) const {
    std::size_t count = 0;
    for (const Call& call : calls) {
      count += call.op == op ? 1 : 0;
    }
    return count;
  }

  core::DisplaySize size{.width = 800, .height = 480};
  bool present = false;
  bool started = false;
  bool powered = false;  // The drive voltages are on.
  bool busy = false;
  bool failWrites = false;
  bool readable = true;  // ReadRows() works.
  int reads = 0;         // Bands read back.
  int refused = 0;
  std::vector<Call> calls;
  std::array<std::vector<std::uint8_t>, 2> memory;      // The controller's two memories,
  std::size_t newIs = 0;                                // and which of them is the new-image plane.
  std::vector<std::uint8_t> shown;                      // What the panel shows.
  std::vector<std::vector<std::uint8_t>> oldAtRefresh;  // The old-image plane at each refresh.
};

}  // namespace bn::test
