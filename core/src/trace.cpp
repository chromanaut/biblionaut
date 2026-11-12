#include "core/trace.h"

#include <algorithm>
#include <atomic>

namespace bn::core {

namespace {

std::atomic<Tracer*> activeTracer{nullptr};

// Holds the tracer's lock for a scope.
class SyncGuard {
 public:
  explicit SyncGuard(TraceSync& sync) noexcept : sync_(sync) { sync_.Lock(); }
  ~SyncGuard() { sync_.Unlock(); }

  SyncGuard(const SyncGuard&) = delete;
  SyncGuard& operator=(const SyncGuard&) = delete;

 private:
  TraceSync& sync_;
};

}  // namespace

void SetActiveTracer(Tracer* tracer) noexcept { activeTracer.store(tracer, std::memory_order_release); }

Tracer* ActiveTracer() noexcept { return activeTracer.load(std::memory_order_acquire); }

Tracer::Tracer(std::span<TraceRecord> storage, Clock& clock, TraceSync& sync) noexcept
    : storage_(storage), clock_(clock), sync_(sync) {}

void Tracer::RecordSpan(SpanId id, std::uint32_t startUs, std::uint32_t arg) noexcept {
  // Unsigned subtraction gives the right duration even if the 32-bit time
  // wrapped during the span.
  const std::uint32_t durationUs = Now() - startUs;
  Write(TraceRecord{.startUs = startUs,
                    .durationUs = durationUs,
                    .arg = arg,
                    .id = id,
                    .task = sync_.CurrentTask(),
                    .kind = TraceKind::kSpan});
}

void Tracer::RecordInstant(SpanId id, std::uint32_t arg) noexcept {
  Write(TraceRecord{.startUs = Now(),
                    .durationUs = 0,
                    .arg = arg,
                    .id = id,
                    .task = sync_.CurrentTask(),
                    .kind = TraceKind::kInstant});
}

void Tracer::Write(const TraceRecord& record) noexcept {
  const std::size_t capacity = storage_.size();
  if (capacity == 0) {
    return;
  }
  const SyncGuard guard(sync_);
  storage_[head_] = record;
  head_ = (head_ + 1) % capacity;
  ++written_;
  if (held_ < capacity) {
    ++held_;
  }
  if (unread_ < capacity) {
    ++unread_;
  } else {
    ++dropped_;  // The ring was full of unread records; the oldest is gone.
  }
}

std::size_t Tracer::Drain(std::span<TraceRecord> out) noexcept {
  const std::size_t capacity = storage_.size();
  if (capacity == 0 || out.empty()) {
    return 0;
  }
  const SyncGuard guard(sync_);
  const std::size_t count = std::min(out.size(), unread_);
  std::size_t slot = (head_ + capacity - unread_) % capacity;
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = storage_[slot];
    slot = (slot + 1) % capacity;
  }
  unread_ -= count;
  return count;
}

std::size_t Tracer::CopyLatest(std::span<TraceRecord> out) noexcept {
  const SyncGuard guard(sync_);
  return CopyLatestUnlocked(out);
}

std::size_t Tracer::CopyLatestUnlocked(std::span<TraceRecord> out) const noexcept {
  const std::size_t capacity = storage_.size();
  if (capacity == 0 || out.empty()) {
    return 0;
  }
  // The values are limited to the ring in case the crash interrupted a write.
  const std::size_t count = std::min({out.size(), held_, capacity});
  std::size_t slot = ((head_ % capacity) + capacity - count) % capacity;
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = storage_[slot];
    slot = (slot + 1) % capacity;
  }
  return count;
}

std::uint32_t Tracer::WrittenCount() noexcept {
  const SyncGuard guard(sync_);
  return written_;
}

std::uint32_t Tracer::DroppedCount() noexcept {
  const SyncGuard guard(sync_);
  return dropped_;
}

}  // namespace bn::core
