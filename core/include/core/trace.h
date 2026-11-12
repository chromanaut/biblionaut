#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/clock.h"

// Timing trace: what ran, when, for how long.
//
// Code marks a stretch of work with BN_TRACE_SCOPE(id). When the scope ends,
// one fixed-size record (id, start, duration, argument, task) goes into a ring
// buffer. No strings and no allocation are involved, so a span costs two clock
// reads and one short critical section. A reader drains the ring and ships
// the records off the device, where ids are turned back into names.
//
// The ring keeps the newest records: if nothing drains it, old records are
// overwritten and counted as dropped. That also makes it a flight recorder,
// since the last moments before a crash are always in it.
//
// How much is traced is a compile-time choice, BN_TRACE_LEVEL:
//   0  nothing: every macro below compiles to nothing
//   1  top-level spans and instants only
//   2  everything (the default)

#ifndef BN_TRACE_LEVEL
#define BN_TRACE_LEVEL 2
#endif

namespace bn::core {

// The part of the program a span id belongs to. Each part numbers its own
// spans, so adding a span never touches another part.
enum class TracePart : std::uint8_t {
  kCore = 0,
  kHw = 1,
  kEpub = 2,
  kDraw = 3,
  kLayout = 4,
  kReader = 5,
};

// Identifies what a record measures. Built with MakeSpanId(); the values are
// part of the trace format, so an id is never renumbered once it is in use.
enum class SpanId : std::uint16_t {};

constexpr SpanId MakeSpanId(TracePart part, std::uint8_t index) noexcept {
  return static_cast<SpanId>((static_cast<unsigned>(part) << 8U) | index);
}

constexpr TracePart PartOf(SpanId id) noexcept { return static_cast<TracePart>(static_cast<std::uint16_t>(id) >> 8U); }

// Task numbers with a fixed meaning. Ordinary tasks are numbered from 0 in
// the order they first record something.
inline constexpr std::uint8_t kTraceTaskOther = 254;      // More tasks than the port can number.
inline constexpr std::uint8_t kTraceTaskInterrupt = 255;  // Recorded from an interrupt handler.

enum class TraceKind : std::uint8_t {
  kSpan = 0,     // Work with a duration.
  kInstant = 1,  // A moment: something happened at startUs.
};

// One trace record. Times are the low 32 bits of the clock's microseconds;
// they wrap about every 71 minutes, which the reader undoes using record
// order.
struct TraceRecord {
  std::uint32_t startUs;
  std::uint32_t durationUs;  // 0 for instants.
  std::uint32_t arg;         // Meaning depends on the id: a byte count, a page number.
  SpanId id;
  std::uint8_t task;  // Which task recorded it, as numbered by TraceSync.
  TraceKind kind;
};
static_assert(sizeof(TraceRecord) == 16);

// What the tracer needs from the system it runs on: a way to keep two tasks
// from writing the ring at once, and a small number for the calling task.
// Implemented by each hardware port.
class TraceSync {
 public:
  virtual void Lock() noexcept = 0;
  virtual void Unlock() noexcept = 0;
  virtual std::uint8_t CurrentTask() noexcept = 0;

 protected:
  ~TraceSync() = default;  // Never destroyed through the interface.
};

// The ring buffer and its two sides: tasks record into it, one reader takes
// records out. The caller owns the storage, so its size is the caller's
// decision.
class Tracer {
 public:
  Tracer(std::span<TraceRecord> storage, Clock& clock, TraceSync& sync) noexcept;

  Tracer(const Tracer&) = delete;
  Tracer& operator=(const Tracer&) = delete;
  ~Tracer() = default;

  // Current time as records store it.
  std::uint32_t Now() noexcept { return static_cast<std::uint32_t>(clock_.NowUs()); }

  // Records a span that started at `startUs` (from Now()) and ends now.
  void RecordSpan(SpanId id, std::uint32_t startUs, std::uint32_t arg) noexcept;

  // Records something that happened now.
  void RecordInstant(SpanId id, std::uint32_t arg) noexcept;

  // Moves the oldest unread records into `out`, oldest first, and returns how
  // many. Each record is handed out once. The copy runs inside the lock, so
  // keep `out` small on a device.
  [[nodiscard]] std::size_t Drain(std::span<TraceRecord> out) noexcept;

  // Copies the newest records into `out`, oldest of them first, without
  // marking them read.
  [[nodiscard]] std::size_t CopyLatest(std::span<TraceRecord> out) noexcept;

  // The same without taking the lock, for a crash handler: everything else
  // has stopped, and the lock may be held by the code that crashed.
  [[nodiscard]] std::size_t CopyLatestUnlocked(std::span<TraceRecord> out) const noexcept;

  std::size_t Capacity() const noexcept { return storage_.size(); }
  // Records ever written.
  std::uint32_t WrittenCount() noexcept;
  // Records overwritten before a reader took them.
  std::uint32_t DroppedCount() noexcept;

 private:
  void Write(const TraceRecord& record) noexcept;

  std::span<TraceRecord> storage_;
  Clock& clock_;
  TraceSync& sync_;
  std::size_t head_ = 0;    // Slot the next record goes into.
  std::size_t held_ = 0;    // Records in the ring, at most its capacity.
  std::size_t unread_ = 0;  // Of those, the newest ones no reader has taken.
  std::uint32_t written_ = 0;
  std::uint32_t dropped_ = 0;
};

// The tracer the macros record into. Null until one is installed, in which
// case the macros do nothing. Install it once at startup, before other tasks
// run.
void SetActiveTracer(Tracer* tracer) noexcept;
Tracer* ActiveTracer() noexcept;

// Records a span covering this object's lifetime. Use through the macros.
class ScopedSpan {
 public:
  explicit ScopedSpan(SpanId id, std::uint32_t arg = 0) noexcept
      : tracer_(ActiveTracer()), startUs_(tracer_ != nullptr ? tracer_->Now() : 0), arg_(arg), id_(id) {}

  ~ScopedSpan() {
    if (tracer_ != nullptr) {
      tracer_->RecordSpan(id_, startUs_, arg_);
    }
  }

  ScopedSpan(const ScopedSpan&) = delete;
  ScopedSpan& operator=(const ScopedSpan&) = delete;

 private:
  Tracer* tracer_;
  std::uint32_t startUs_;
  std::uint32_t arg_;
  SpanId id_;
};

// Records an instant on the active tracer, if there is one.
inline void TraceInstant(SpanId id, std::uint32_t arg = 0) noexcept {
  if (Tracer* tracer = ActiveTracer()) {
    tracer->RecordInstant(id, arg);
  }
}

// For a span that starts in one place and ends in another, such as work
// begun by a task and finished in an interrupt handler: note TraceNow() at
// the start, and pass it to TraceSpanSince() at the end.
inline std::uint32_t TraceNow() noexcept {
  Tracer* tracer = ActiveTracer();
  return tracer != nullptr ? tracer->Now() : 0;
}

inline void TraceSpanSince(SpanId id, std::uint32_t startUs, std::uint32_t arg = 0) noexcept {
  if (Tracer* tracer = ActiveTracer()) {
    tracer->RecordSpan(id, startUs, arg);
  }
}

}  // namespace bn::core

#define BN_TRACE_CONCAT_INNER(a, b) a##b
#define BN_TRACE_CONCAT(a, b) BN_TRACE_CONCAT_INNER(a, b)
#define BN_TRACE_SCOPE_IMPL(id, arg) const ::bn::core::ScopedSpan BN_TRACE_CONCAT(bnTraceSpan, __LINE__)((id), (arg))
// What a compiled-out macro becomes: a lambda that mentions the arguments and
// is never called. The arguments are not evaluated, but they still count as
// used, so a value computed only for the trace does not turn into an
// unused-variable error when tracing is off.
#define BN_TRACE_IGNORE(id) static_cast<void>([&] { static_cast<void>(id); })
#define BN_TRACE_IGNORE_ARG(id, arg) \
  static_cast<void>([&] {            \
    static_cast<void>(id);           \
    static_cast<void>(arg);          \
  })

// A span for the rest of the enclosing scope. The _ARG forms attach a number
// known at the start (a size, an index). When a macro is compiled out its
// arguments are not evaluated.
//
// TOP_SCOPE is for the few spans that make up the outline of an interaction
// (lay out a page, refresh the panel); SCOPE is for the detail inside them.
// INSTANT marks a moment. TOP_SPAN_SINCE ends a top-level span that began at
// a time taken earlier with BN_TRACE_NOW(), possibly somewhere else.
#if BN_TRACE_LEVEL >= 1
#define BN_TRACE_TOP_SCOPE(id) BN_TRACE_SCOPE_IMPL(id, 0)
#define BN_TRACE_TOP_SCOPE_ARG(id, arg) BN_TRACE_SCOPE_IMPL(id, arg)
#define BN_TRACE_INSTANT(id, arg) ::bn::core::TraceInstant((id), (arg))
#define BN_TRACE_NOW() ::bn::core::TraceNow()
#define BN_TRACE_TOP_SPAN_SINCE(id, startUs, arg) ::bn::core::TraceSpanSince((id), (startUs), (arg))
#else
#define BN_TRACE_TOP_SCOPE(id) BN_TRACE_IGNORE(id)
#define BN_TRACE_TOP_SCOPE_ARG(id, arg) BN_TRACE_IGNORE_ARG(id, arg)
#define BN_TRACE_INSTANT(id, arg) BN_TRACE_IGNORE_ARG(id, arg)
#define BN_TRACE_NOW() (std::uint32_t{0})
#define BN_TRACE_TOP_SPAN_SINCE(id, startUs, arg) \
  static_cast<void>([&] {                         \
    static_cast<void>(id);                        \
    static_cast<void>(startUs);                   \
    static_cast<void>(arg);                       \
  })
#endif

#if BN_TRACE_LEVEL >= 2
#define BN_TRACE_SCOPE(id) BN_TRACE_SCOPE_IMPL(id, 0)
#define BN_TRACE_SCOPE_ARG(id, arg) BN_TRACE_SCOPE_IMPL(id, arg)
#else
#define BN_TRACE_SCOPE(id) BN_TRACE_IGNORE(id)
#define BN_TRACE_SCOPE_ARG(id, arg) BN_TRACE_IGNORE_ARG(id, arg)
#endif
