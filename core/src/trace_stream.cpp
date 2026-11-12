#include "core/trace_stream.h"

namespace bn::core {

namespace {

// Frames sent by one Pump() call, so a long backlog cannot hold the caller.
constexpr int kMaxFramesPerPump = 4;

std::span<const std::uint8_t> AsBytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace

bool TraceStream::SendFrame(FrameWriter& writer) noexcept {
  const std::span<const std::uint8_t> frame = writer.Finish();
  if (frame.empty() || !sink_.Write(frame)) {
    lostFrames_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool TraceStream::SendTextFrame(FrameType type, std::uint32_t timeUs, std::string_view text) noexcept {
  // Not substr(): it can throw, which drags the C++ library's exception
  // support into the firmware.
  if (text.size() > kMaxTextLength) {
    text.remove_suffix(text.size() - kMaxTextLength);
  }
  std::array<std::uint8_t, MaxEncodedFrameSize(4 + kMaxTextLength)> buffer{};
  FrameWriter writer(type, buffer);
  writer.PutU32(timeUs);
  writer.Put(AsBytes(text));
  return SendFrame(writer);
}

bool TraceStream::SendHello(std::string_view version) noexcept {
  if (version.size() > kMaxTextLength) {
    version.remove_suffix(version.size() - kMaxTextLength);
  }
  std::array<std::uint8_t, MaxEncodedFrameSize(1 + kMaxTextLength)> buffer{};
  FrameWriter writer(FrameType::kHello, buffer);
  writer.Put(kStreamProtocolVersion);
  writer.Put(AsBytes(version));
  return SendFrame(writer);
}

bool TraceStream::SendText(std::string_view text) noexcept {
  return SendTextFrame(FrameType::kText, tracer_.Now(), text);
}

bool TraceStream::SendMetrics(const MetricRegistry& registry) noexcept {
  const std::size_t total = registry.CounterCount() + registry.GaugeCount();
  const std::uint32_t now = tracer_.Now();
  bool allSent = true;
  std::size_t first = 0;
  do {
    std::array<char, kMaxTextLength + 1> line{};
    const MetricRegistry::Formatted formatted = registry.FormatItems(line, first);
    allSent = SendTextFrame(FrameType::kMetrics, now, std::string_view(line.data(), formatted.length)) && allSent;
    // A metric too long for any frame is skipped, so the rest still go out.
    first += formatted.count > 0 ? formatted.count : 1;
  } while (first < total);
  return allSent;
}

void TraceStream::EncodePendingTraceFrame(std::size_t count) noexcept {
  FrameWriter writer(FrameType::kTrace, pending_);
  // Lets the reader see that records went missing before this frame.
  writer.PutU32(tracer_.DroppedCount());
  for (const TraceRecord& record : std::span(batch_).first(count)) {
    writer.PutU32(record.startUs);
    writer.PutU32(record.durationUs);
    writer.PutU32(record.arg);
    writer.PutU16(static_cast<std::uint16_t>(record.id));
    writer.Put(record.task);
    writer.Put(static_cast<std::uint8_t>(record.kind));
  }
  pendingSize_ = writer.Finish().size();
  pendingRecords_ = count;
}

std::size_t TraceStream::Pump() noexcept {
  std::size_t sent = 0;
  for (int frames = 0; frames < kMaxFramesPerPump; ++frames) {
    if (pendingSize_ == 0) {
      const std::size_t count = tracer_.Drain(batch_);
      if (count == 0) {
        break;
      }
      EncodePendingTraceFrame(count);
    }
    if (!sink_.Write(std::span(pending_).first(pendingSize_))) {
      break;  // Keep the frame for the next call.
    }
    sent += pendingRecords_;
    pendingSize_ = 0;
    pendingRecords_ = 0;
  }
  return sent;
}

}  // namespace bn::core
