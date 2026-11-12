#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "core/display.h"

namespace bn::test {

// A panel that records what is asked of it. It is absent (Start() fails)
// unless the test says otherwise, and busy only when the test says so.
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

  FakeDisplay() : newPlane(size.RowBytes() * size.height, 0xFF), oldPlane(newPlane) {}

  core::DisplaySize Size() const noexcept override { return size; }

  [[nodiscard]] bool Start() noexcept override {
    calls.push_back({.op = Op::kStart});
    started = present;
    busy = false;  // Setting a panel up stops whatever it was doing.
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
    std::vector<std::uint8_t>& target = plane == core::DisplayPlane::kNew ? newPlane : oldPlane;
    std::memcpy(target.data() + (firstRow * rowBytes), rows.data(), rows.size());
    return true;
  }

  [[nodiscard]] bool StartRefresh(core::RefreshMode mode, core::AfterRefresh after) noexcept override {
    if (!started || busy) {
      ++refused;
      return false;
    }
    calls.push_back({.op = Op::kRefresh, .mode = mode, .after = after});
    powered = after == core::AfterRefresh::kStayPowered;
    // What the panel will show, and both planes as they were when asked.
    shownAtRefresh.push_back(newPlane);
    oldAtRefresh.push_back(oldPlane);
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
  int refused = 0;
  std::vector<Call> calls;
  std::vector<std::uint8_t> newPlane;
  std::vector<std::uint8_t> oldPlane;
  std::vector<std::vector<std::uint8_t>> shownAtRefresh;  // The new-image plane at each refresh.
  std::vector<std::vector<std::uint8_t>> oldAtRefresh;    // The old-image plane at each refresh.
};

}  // namespace bn::test
