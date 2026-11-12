#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/byte_sink.h"
#include "core/endian.h"
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

  // The payload as text: all of it for raw bytes, and what follows the
  // leading field for a frame (the version byte of a hello, the time of a
  // text or metrics frame).
  std::string Text() const {
    std::size_t skip = 0;
    if (isFrame) {
      skip = type == core::FrameType::kHello ? 1 : 4;
    }
    if (skip > payload.size()) {
      return {};
    }
    return {payload.begin() + static_cast<std::ptrdiff_t>(skip), payload.end()};
  }

  // The time a text or metrics frame was sent.
  std::uint32_t TimeUs() const {
    if (payload.size() < 4) {
      return 0;
    }
    return core::LoadLe32(std::span<const std::uint8_t>(payload).first<4>());
  }
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
