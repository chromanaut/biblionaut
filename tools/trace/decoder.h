#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/trace.h"

// Turns the bytes of a diagnostic stream (core/frame.h, core/trace_stream.h)
// back into events with full-width times.

namespace bn::trace_tool {

// The protocol version this tool understands (core::kStreamProtocolVersion).
inline constexpr std::uint8_t kSupportedProtocol = 2;

enum class EventKind : std::uint8_t {
  kHello,    // The device started a stream: `text` is its version, `arg` the protocol version.
  kSpan,     // A trace span.
  kInstant,  // A trace instant.
  kText,     // A log line sent as a frame.
  kMetrics,  // The counters-and-gauges line.
  kRawText,  // A line of plain text that was not in a frame (boot messages, a crash dump).
  kDropped,  // Trace records were lost on the device before this point; `arg` is how many.
};

struct Event {
  EventKind kind = EventKind::kRawText;
  int boot = 0;                  // Which run of the device this belongs to; 0 before any hello.
  std::uint64_t timeUs = 0;      // Device time since boot. For a span, when it started.
  std::uint64_t durationUs = 0;  // Spans only.
  core::SpanId id{};             // Spans and instants.
  std::uint8_t task = 0;         // Spans and instants.
  std::uint32_t arg = 0;
  std::string text;
};

struct DecoderStats {
  std::size_t bytes = 0;
  std::size_t frames = 0;
  std::size_t records = 0;
  std::size_t droppedRecords = 0;  // Lost on the device (its ring was full).
  std::size_t rawTextLines = 0;
  std::size_t unsupportedFrames = 0;  // Unknown frame type, malformed payload, or wrong protocol.
  int boots = 0;
};

class StreamDecoder {
 public:
  // Accepts the next bytes of the stream, in any size of pieces.
  void Feed(std::span<const std::uint8_t> bytes);

  // Call at the end of the stream: decodes whatever is still buffered.
  void Finish();

  // Returns the events decoded since the last call.
  std::vector<Event> TakeEvents();

  const DecoderStats& Stats() const noexcept { return stats_; }

 private:
  void EndPiece();
  void EmitRawText(std::span<const std::uint8_t> bytes, bool complete);
  void HandleFrame(std::uint8_t type, std::span<const std::uint8_t> payload);
  void HandleTrace(std::span<const std::uint8_t> payload);
  std::uint64_t Unwrap(std::uint32_t timeUs);

  std::vector<std::uint8_t> piece_;  // Bytes since the last zero.
  std::string rawLine_;              // An unfinished line of plain text.
  bool seenZero_ = false;            // Whether any frame delimiter has been seen yet.
  bool pieceIsText_ = false;         // The current piece is too long to be a frame.
  std::vector<Event> events_;
  DecoderStats stats_;

  // The current boot's clock: device times are 32 bits and wrap, so each is
  // placed relative to the latest time seen.
  int boot_ = 0;
  bool protocolSupported_ = true;
  bool haveTime_ = false;
  std::uint32_t lastTime32_ = 0;
  std::uint64_t lastTime64_ = 0;
  std::uint32_t lastDropped_ = 0;
};

// Makes text safe to print and to put in JSON: valid UTF-8 passes through,
// and control characters and stray bytes become \xNN.
std::string SanitizeText(std::span<const std::uint8_t> bytes);

// After the tool restarts a device, the first things to arrive are left over
// from before the restart: trace the device had buffered, possibly a frame
// cut in half. This drops events until the restarted device announces itself,
// by the first line its boot ROM prints or, failing that, its hello.
class BootFilter {
 public:
  // Returns true if the event belongs to the restarted device and should be
  // kept. Once one is kept, all later ones are.
  bool Keep(const Event& event) noexcept;

  // Stops waiting: keep everything from now on. For when no boot ever shows.
  void GiveUp() noexcept { waiting_ = false; }

  bool Waiting() const noexcept { return waiting_; }
  std::size_t Skipped() const noexcept { return skipped_; }

 private:
  bool waiting_ = true;
  std::size_t skipped_ = 0;
};

}  // namespace bn::trace_tool
