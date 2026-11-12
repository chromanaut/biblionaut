#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/byte_sink.h"
#include "core/frame.h"

namespace bn::test {

// Collects what was written. Can be told to refuse writes, like a serial
// buffer with no room.
class FakeSink final : public core::ByteSink {
 public:
  [[nodiscard]] bool Write(std::span<const std::uint8_t> bytes) noexcept override {
    if (full) {
      ++refused;
      return false;
    }
    received.insert(received.end(), bytes.begin(), bytes.end());
    ++writes;
    return true;
  }

  std::vector<std::uint8_t> received;
  bool full = false;
  int writes = 0;
  int refused = 0;
};

// One thing found on the line: a frame, or a run of bytes that is not one.
struct StreamItem {
  bool isFrame = false;
  core::FrameType type = core::FrameType::kText;
  std::vector<std::uint8_t> payload;  // The frame's payload, or the raw bytes.

  std::string Text() const { return {payload.begin(), payload.end()}; }
};

// Splits a captured byte stream the way a host-side reader does: cut at zero
// bytes, and treat each piece as a frame if it decodes, as raw bytes if not.
inline std::vector<StreamItem> ParseStream(std::span<const std::uint8_t> bytes) {
  std::vector<StreamItem> items;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= bytes.size(); ++i) {
    if (i < bytes.size() && bytes[i] != 0) {
      continue;
    }
    const std::span<const std::uint8_t> piece = bytes.subspan(start, i - start);
    start = i + 1;
    if (piece.empty()) {
      continue;
    }
    std::vector<std::uint8_t> scratch(piece.size());
    StreamItem item;
    if (const auto frame = core::DecodeFrame(piece, scratch)) {
      item.isFrame = true;
      item.type = frame->type;
      item.payload.assign(frame->payload.begin(), frame->payload.end());
    } else {
      item.payload.assign(piece.begin(), piece.end());
    }
    items.push_back(std::move(item));
  }
  return items;
}

}  // namespace bn::test
