#include "trace/decoder.h"

#include <array>
#include <cstdio>
#include <optional>
#include <utility>

#include "core/endian.h"
#include "core/frame.h"
#include "core/trace_stream.h"

namespace bn::trace_tool {

namespace {

// Bytes between two delimiters beyond which the piece cannot be a frame: the
// largest frame the device sends is a full trace frame.
constexpr std::size_t kMaxFramePiece =
    core::MaxEncodedFrameSize(4 + (core::kMaxRecordsPerFrame * core::kWireRecordSize)) + 64;

constexpr std::size_t kTimeSize = 4;

void AppendEscaped(std::string& out, std::uint8_t byte) {
  std::array<char, 8> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "\\x%02X", static_cast<unsigned>(byte));
  out += buffer.data();
}

// Length of the valid UTF-8 sequence starting at bytes[0], or 0 if there is
// none (a stray continuation byte, a truncated or overlong sequence).
std::size_t Utf8SequenceLength(std::span<const std::uint8_t> bytes) {
  const std::uint8_t lead = bytes[0];
  std::size_t length = 0;
  std::uint32_t minimum = 0;
  std::uint32_t value = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    minimum = 0x80;
    value = lead & 0x1FU;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    minimum = 0x800;
    value = lead & 0x0FU;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    minimum = 0x10000;
    value = lead & 0x07U;
  } else {
    return 0;
  }
  if (bytes.size() < length) {
    return 0;
  }
  for (std::size_t i = 1; i < length; ++i) {
    if ((bytes[i] & 0xC0U) != 0x80U) {
      return 0;
    }
    value = (value << 6U) | (bytes[i] & 0x3FU);
  }
  const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
  if (value < minimum || value > 0x10FFFF || surrogate) {
    return 0;
  }
  return length;
}

}  // namespace

std::string SanitizeText(std::span<const std::uint8_t> bytes) {
  std::string out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    const std::uint8_t byte = bytes[i];
    if (byte == '\t' || (byte >= 0x20 && byte <= 0x7E)) {
      out += static_cast<char>(byte);
      ++i;
      continue;
    }
    if (byte >= 0x80) {
      const std::size_t length = Utf8SequenceLength(bytes.subspan(i));
      if (length > 0) {
        out.append(reinterpret_cast<const char*>(bytes.data() + i), length);
        i += length;
        continue;
      }
    }
    // A control character, or a byte that is not part of valid UTF-8.
    AppendEscaped(out, byte);
    ++i;
  }
  return out;
}

bool BootFilter::Keep(const Event& event) noexcept {
  if (!waiting_) {
    return true;
  }
  const bool romBanner = event.kind == EventKind::kRawText && event.text.starts_with("ESP-ROM:");
  if (romBanner || event.kind == EventKind::kHello) {
    waiting_ = false;
    return true;
  }
  ++skipped_;
  return false;
}

void StreamDecoder::Feed(std::span<const std::uint8_t> bytes) {
  stats_.bytes += bytes.size();
  for (const std::uint8_t byte : bytes) {
    if (byte == 0) {
      EndPiece();
      seenZero_ = true;
      continue;
    }
    piece_.push_back(byte);
    // Before the first delimiter nothing can be a frame, and a piece longer
    // than the longest frame is not one either: treat both as plain text and
    // show its lines as they complete, without waiting for a delimiter.
    if (piece_.size() > kMaxFramePiece) {
      pieceIsText_ = true;
    }
    if ((pieceIsText_ || !seenZero_) && byte == '\n') {
      EmitRawText(piece_, false);
      piece_.clear();
    }
  }
}

void StreamDecoder::Finish() {
  // What is left has no closing delimiter, so it is not a frame.
  EmitRawText(piece_, true);
  piece_.clear();
}

std::vector<Event> StreamDecoder::TakeEvents() { return std::exchange(events_, {}); }

void StreamDecoder::EndPiece() {
  const bool plainText = pieceIsText_ || !seenZero_;
  pieceIsText_ = false;
  if (!plainText && !piece_.empty()) {
    std::vector<std::uint8_t> scratch(piece_.size());
    if (const std::optional<core::Frame> frame = core::DecodeFrame(piece_, scratch)) {
      HandleFrame(static_cast<std::uint8_t>(frame->type), frame->payload);
      piece_.clear();
      return;
    }
  }
  EmitRawText(piece_, true);
  piece_.clear();
}

void StreamDecoder::EmitRawText(std::span<const std::uint8_t> bytes, bool complete) {
  const auto emitLine = [this] {
    if (!rawLine_.empty() && rawLine_.back() == '\r') {
      rawLine_.pop_back();
    }
    Event event;
    event.kind = EventKind::kRawText;
    event.boot = boot_;
    event.timeUs = lastTime64_;
    event.text = SanitizeText({reinterpret_cast<const std::uint8_t*>(rawLine_.data()), rawLine_.size()});
    events_.push_back(std::move(event));
    ++stats_.rawTextLines;
    rawLine_.clear();
  };

  for (const std::uint8_t byte : bytes) {
    if (byte == '\n') {
      emitLine();
    } else {
      rawLine_ += static_cast<char>(byte);
    }
  }
  if (complete && !rawLine_.empty()) {
    emitLine();
  }
}

std::uint64_t StreamDecoder::Unwrap(std::uint32_t timeUs) {
  if (!haveTime_) {
    haveTime_ = true;
    lastTime32_ = timeUs;
    lastTime64_ = timeUs;
    return lastTime64_;
  }
  // The difference from the latest time seen, taken as signed, is right as
  // long as the two are within about 35 minutes of each other.
  const auto delta = static_cast<std::int32_t>(timeUs - lastTime32_);
  if (delta >= 0) {
    lastTime64_ += static_cast<std::uint64_t>(delta);
    lastTime32_ = timeUs;
    return lastTime64_;
  }
  const auto back = static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta));
  return back > lastTime64_ ? 0 : lastTime64_ - back;
}

void StreamDecoder::HandleFrame(std::uint8_t type, std::span<const std::uint8_t> payload) {
  ++stats_.frames;
  const auto frameType = static_cast<core::FrameType>(type);

  if (frameType == core::FrameType::kHello) {
    if (payload.empty()) {
      ++stats_.unsupportedFrames;
      return;
    }
    // A hello means the device (re)started: its clock begins again.
    ++boot_;
    stats_.boots = boot_;
    haveTime_ = false;
    lastTime32_ = 0;
    lastTime64_ = 0;
    lastDropped_ = 0;
    protocolSupported_ = payload[0] == kSupportedProtocol;
    AbandonCrashReport();  // The restart cut it short; the new run sends it again.

    Event event;
    event.kind = EventKind::kHello;
    event.boot = boot_;
    event.arg = payload[0];
    event.text = SanitizeText(payload.subspan(1));
    events_.push_back(std::move(event));
    return;
  }

  if (!protocolSupported_) {
    ++stats_.unsupportedFrames;
    return;
  }

  switch (frameType) {
    case core::FrameType::kTrace:
      HandleTrace(payload);
      return;
    case core::FrameType::kText:
    case core::FrameType::kMetrics: {
      if (payload.size() < kTimeSize) {
        ++stats_.unsupportedFrames;
        return;
      }
      Event event;
      event.kind = frameType == core::FrameType::kText ? EventKind::kText : EventKind::kMetrics;
      event.boot = boot_;
      event.timeUs = Unwrap(core::LoadLe32(payload.first<kTimeSize>()));
      event.text = SanitizeText(payload.subspan(kTimeSize));
      events_.push_back(std::move(event));
      return;
    }
    case core::FrameType::kCrash:
      HandleCrashPiece(payload);
      return;
    case core::FrameType::kHello:
      return;
  }
  ++stats_.unsupportedFrames;
}

void StreamDecoder::AbandonCrashReport() {
  if (crashTotal_ != 0) {
    ++stats_.damagedCrashReports;
  }
  crashBytes_.clear();
  crashTotal_ = 0;
}

void StreamDecoder::HandleCrashPiece(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kHeaderSize = 4;
  if (payload.size() <= kHeaderSize) {
    ++stats_.unsupportedFrames;
    return;
  }
  const std::size_t offset = core::LoadLe16(payload.subspan(0).first<2>());
  const std::size_t total = core::LoadLe16(payload.subspan(2).first<2>());
  const std::span<const std::uint8_t> bytes = payload.subspan(kHeaderSize);

  if (offset == 0) {
    AbandonCrashReport();
    crashTotal_ = total;
  }
  // Pieces come in order. One that does not continue what is here means a
  // piece was lost, and the rest cannot be used.
  if (crashTotal_ == 0 || total != crashTotal_ || offset != crashBytes_.size() || offset + bytes.size() > total) {
    AbandonCrashReport();
    return;
  }
  crashBytes_.insert(crashBytes_.end(), bytes.begin(), bytes.end());
  if (crashBytes_.size() < crashTotal_) {
    return;
  }

  auto report = std::make_shared<core::CrashReport>();
  const bool good = core::DecodeCrashReport(crashBytes_, *report);
  crashBytes_.clear();
  crashTotal_ = 0;
  if (!good) {
    ++stats_.damagedCrashReports;
    return;
  }
  ++stats_.crashReports;
  Event event;
  event.kind = EventKind::kCrash;
  event.boot = boot_;
  event.timeUs = lastTime64_;
  event.text = std::string(core::CrashText(report->reason));
  event.crash = std::move(report);
  events_.push_back(std::move(event));
}

void StreamDecoder::HandleTrace(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kHeaderSize = 4;
  if (payload.size() < kHeaderSize || (payload.size() - kHeaderSize) % core::kWireRecordSize != 0) {
    ++stats_.unsupportedFrames;
    return;
  }

  const std::uint32_t dropped = core::LoadLe32(payload.first<kHeaderSize>());
  if (dropped > lastDropped_) {
    Event event;
    event.kind = EventKind::kDropped;
    event.boot = boot_;
    event.timeUs = lastTime64_;
    event.arg = dropped - lastDropped_;
    stats_.droppedRecords += event.arg;
    events_.push_back(std::move(event));
  }
  lastDropped_ = dropped;

  for (std::size_t at = kHeaderSize; at < payload.size(); at += core::kWireRecordSize) {
    const std::span<const std::uint8_t> record = payload.subspan(at, core::kWireRecordSize);
    const std::uint32_t start = core::LoadLe32(record.subspan(0).first<4>());
    const std::uint32_t duration = core::LoadLe32(record.subspan(4).first<4>());

    Event event;
    event.kind =
        record[15] == static_cast<std::uint8_t>(core::TraceKind::kSpan) ? EventKind::kSpan : EventKind::kInstant;
    event.boot = boot_;
    event.arg = core::LoadLe32(record.subspan(8).first<4>());
    event.id = static_cast<core::SpanId>(core::LoadLe16(record.subspan(12).first<2>()));
    event.task = record[14];
    // A span is recorded when it ends, so the end is what follows on from the
    // previous record; place that, then step back to the start.
    const std::uint64_t end = Unwrap(start + duration);
    event.durationUs = event.kind == EventKind::kSpan ? duration : 0;
    event.timeUs = end >= event.durationUs ? end - event.durationUs : 0;
    events_.push_back(std::move(event));
    ++stats_.records;
  }
}

}  // namespace bn::trace_tool
