#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/byte_sink.h"
#include "core/frame.h"
#include "core/metrics.h"
#include "core/trace.h"

// The diagnostic stream: trace records, log text and metrics, sent as frames
// (core/frame.h) to a ByteSink such as the USB serial port.
//
// Nothing here waits. If the sink has no room, trace records stay in the
// tracer's ring until a later Pump(), and a log line or metrics line is
// dropped and counted.

namespace bn::core {

// Bumped whenever a frame's layout changes.
inline constexpr std::uint8_t kStreamProtocolVersion = 1;

// Trace records carried by one frame.
inline constexpr std::size_t kMaxRecordsPerFrame = 16;
// Size of one record on the wire: start, duration, argument, id, task, kind.
inline constexpr std::size_t kWireRecordSize = 16;
// Longest log or metrics text one frame carries; longer text is cut.
inline constexpr std::size_t kMaxTextLength = 192;

class TraceStream {
 public:
  TraceStream(Tracer& tracer, ByteSink& sink) noexcept : tracer_(tracer), sink_(sink) {}

  TraceStream(const TraceStream&) = delete;
  TraceStream& operator=(const TraceStream&) = delete;
  ~TraceStream() = default;

  // Announces the stream: protocol version and the firmware version text.
  // Returns false if the sink had no room.
  bool SendHello(std::string_view version) noexcept;

  // Sends one line of log text. Safe to call from any task.
  bool SendText(std::string_view text) noexcept;

  // Sends every counter and gauge of `registry` as one line.
  bool SendMetrics(const MetricRegistry& registry) noexcept;

  // Moves trace records from the tracer to the sink, oldest first, until the
  // tracer is empty, the sink is full, or a per-call limit is reached.
  // Returns how many records were sent. Call from one task only.
  std::size_t Pump() noexcept;

  // Hello, text and metrics frames lost because the sink had no room.
  std::uint32_t LostFrames() const noexcept { return lostFrames_.load(std::memory_order_relaxed); }

 private:
  bool SendTextFrame(FrameType type, std::string_view text) noexcept;
  void EncodePendingTraceFrame(std::size_t count) noexcept;

  Tracer& tracer_;
  ByteSink& sink_;
  std::atomic<std::uint32_t> lostFrames_{0};

  // The trace frame being sent. It is kept across calls when the sink is
  // full, so records taken out of the tracer are never lost.
  std::array<TraceRecord, kMaxRecordsPerFrame> batch_{};
  std::array<std::uint8_t, MaxEncodedFrameSize(4 + (kMaxRecordsPerFrame * kWireRecordSize))> pending_{};
  std::size_t pendingSize_ = 0;
  std::size_t pendingRecords_ = 0;
};

}  // namespace bn::core
