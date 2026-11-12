// These tests need every macro live, whatever trace level the build chose.
#undef BN_TRACE_LEVEL
#define BN_TRACE_LEVEL 2
#include "core/trace.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "trace_fakes.h"

namespace {

using bn::core::MakeSpanId;
using bn::core::SpanId;
using bn::core::TraceKind;
using bn::core::TracePart;
using bn::core::TraceRecord;
using bn::test::kSpanInner;
using bn::test::kSpanMark;
using bn::test::kSpanOuter;
using bn::test::TraceFixture;

SpanId IdOf(std::uint8_t index) { return MakeSpanId(TracePart::kCore, index); }

TEST(SpanId, CombinesPartAndIndex) {
  const SpanId id = MakeSpanId(TracePart::kLayout, 3);
  EXPECT_EQ(static_cast<std::uint16_t>(id), 0x0403u);
  EXPECT_EQ(bn::core::PartOf(id), TracePart::kLayout);
  EXPECT_NE(MakeSpanId(TracePart::kEpub, 3), MakeSpanId(TracePart::kDraw, 3));
}

TEST(Tracer, RecordsASpanWithItsTimesArgumentAndTask) {
  TraceFixture<8> fixture;
  fixture.sync.task = 3;
  fixture.clock.now = 1'000;
  {
    BN_TRACE_SCOPE_ARG(kSpanOuter, 42);
    fixture.clock.now = 1'250;
  }

  std::array<TraceRecord, 4> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 1u);
  EXPECT_EQ(out[0].id, kSpanOuter);
  EXPECT_EQ(out[0].kind, TraceKind::kSpan);
  EXPECT_EQ(out[0].startUs, 1'000u);
  EXPECT_EQ(out[0].durationUs, 250u);
  EXPECT_EQ(out[0].arg, 42u);
  EXPECT_EQ(out[0].task, 3);
}

TEST(Tracer, NestedSpansAreRecordedInnerFirst) {
  TraceFixture<8> fixture;
  fixture.clock.now = 100;
  {
    BN_TRACE_TOP_SCOPE(kSpanOuter);
    fixture.clock.now = 110;
    {
      BN_TRACE_SCOPE(kSpanInner);
      fixture.clock.now = 150;
    }
    fixture.clock.now = 200;
  }

  std::array<TraceRecord, 4> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 2u);
  EXPECT_EQ(out[0].id, kSpanInner);
  EXPECT_EQ(out[0].startUs, 110u);
  EXPECT_EQ(out[0].durationUs, 40u);
  EXPECT_EQ(out[1].id, kSpanOuter);
  EXPECT_EQ(out[1].startUs, 100u);
  EXPECT_EQ(out[1].durationUs, 100u);
}

TEST(Tracer, RecordsAnInstant) {
  TraceFixture<8> fixture;
  fixture.clock.now = 5'000;
  BN_TRACE_INSTANT(kSpanMark, 9);

  std::array<TraceRecord, 2> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 1u);
  EXPECT_EQ(out[0].id, kSpanMark);
  EXPECT_EQ(out[0].kind, TraceKind::kInstant);
  EXPECT_EQ(out[0].startUs, 5'000u);
  EXPECT_EQ(out[0].durationUs, 0u);
  EXPECT_EQ(out[0].arg, 9u);
}

TEST(Tracer, DurationIsRightWhenTheClockWrapsDuringASpan) {
  TraceFixture<4> fixture;
  fixture.clock.now = 0xFFFF'FFF0u;
  const std::uint32_t start = fixture.tracer.Now();
  fixture.clock.now = 0x1'0000'0010u;  // 32 microseconds later, past the 32-bit wrap.
  fixture.tracer.RecordSpan(kSpanOuter, start, 0);

  std::array<TraceRecord, 1> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 1u);
  EXPECT_EQ(out[0].startUs, 0xFFFF'FFF0u);
  EXPECT_EQ(out[0].durationUs, 32u);
}

TEST(Tracer, MacrosDoNothingWithoutAnActiveTracer) {
  bn::core::SetActiveTracer(nullptr);
  {
    BN_TRACE_TOP_SCOPE(kSpanOuter);
    BN_TRACE_SCOPE(kSpanInner);
    BN_TRACE_INSTANT(kSpanMark, 1);
  }
  EXPECT_EQ(bn::core::ActiveTracer(), nullptr);
}

TEST(Tracer, ASpanOpenedBeforeTheTracerWasRemovedStillRecordsToIt) {
  TraceFixture<4> fixture;
  {
    BN_TRACE_SCOPE(kSpanOuter);
    bn::core::SetActiveTracer(nullptr);
  }
  EXPECT_EQ(fixture.tracer.WrittenCount(), 1u);
}

TEST(Tracer, DrainHandsEachRecordOutOnceInOrder) {
  TraceFixture<8> fixture;
  for (std::uint8_t i = 0; i < 5; ++i) {
    fixture.tracer.RecordInstant(IdOf(i), i);
  }

  std::array<TraceRecord, 2> small{};
  ASSERT_EQ(fixture.tracer.Drain(small), 2u);
  EXPECT_EQ(small[0].arg, 0u);
  EXPECT_EQ(small[1].arg, 1u);

  std::array<TraceRecord, 8> rest{};
  ASSERT_EQ(fixture.tracer.Drain(rest), 3u);
  EXPECT_EQ(rest[0].arg, 2u);
  EXPECT_EQ(rest[2].arg, 4u);

  EXPECT_EQ(fixture.tracer.Drain(rest), 0u);

  fixture.tracer.RecordInstant(IdOf(9), 9);
  ASSERT_EQ(fixture.tracer.Drain(rest), 1u);
  EXPECT_EQ(rest[0].arg, 9u);
}

TEST(Tracer, AFullRingKeepsTheNewestRecordsAndCountsTheRest) {
  TraceFixture<4> fixture;
  for (std::uint8_t i = 0; i < 6; ++i) {
    fixture.tracer.RecordInstant(IdOf(i), i);
  }
  EXPECT_EQ(fixture.tracer.WrittenCount(), 6u);
  EXPECT_EQ(fixture.tracer.DroppedCount(), 2u);

  std::array<TraceRecord, 8> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 4u);
  EXPECT_EQ(out[0].arg, 2u);
  EXPECT_EQ(out[3].arg, 5u);
}

TEST(Tracer, RecordsReadBeforeTheRingFillsAreNotCountedAsDropped) {
  TraceFixture<4> fixture;
  std::array<TraceRecord, 4> out{};
  for (std::uint8_t round = 0; round < 5; ++round) {
    for (std::uint8_t i = 0; i < 3; ++i) {
      fixture.tracer.RecordInstant(IdOf(i), static_cast<std::uint32_t>(round * 10 + i));
    }
    ASSERT_EQ(fixture.tracer.Drain(out), 3u);
    EXPECT_EQ(out[0].arg, static_cast<std::uint32_t>(round * 10));
    EXPECT_EQ(out[2].arg, static_cast<std::uint32_t>(round * 10 + 2));
  }
  EXPECT_EQ(fixture.tracer.WrittenCount(), 15u);
  EXPECT_EQ(fixture.tracer.DroppedCount(), 0u);
}

TEST(Tracer, CopyLatestReturnsTheNewestWithoutConsumingThem) {
  TraceFixture<4> fixture;
  for (std::uint8_t i = 0; i < 6; ++i) {
    fixture.tracer.RecordInstant(IdOf(i), i);
  }

  std::array<TraceRecord, 3> latest{};
  ASSERT_EQ(fixture.tracer.CopyLatest(latest), 3u);
  EXPECT_EQ(latest[0].arg, 3u);
  EXPECT_EQ(latest[2].arg, 5u);

  std::array<TraceRecord, 8> out{};
  EXPECT_EQ(fixture.tracer.Drain(out), 4u);
}

TEST(Tracer, CopyLatestUnlockedGivesTheSameWithoutTheLock) {
  TraceFixture<4> fixture;
  for (std::uint8_t i = 0; i < 6; ++i) {
    fixture.tracer.RecordInstant(IdOf(i), i);
  }
  const int locksBefore = fixture.sync.lockCount;

  // A crash handler may run while the lock is held; it must not wait for it.
  std::array<TraceRecord, 3> latest{};
  ASSERT_EQ(fixture.tracer.CopyLatestUnlocked(latest), 3u);
  EXPECT_EQ(latest[0].arg, 3u);
  EXPECT_EQ(latest[2].arg, 5u);
  EXPECT_EQ(fixture.sync.lockCount, locksBefore);

  // Asking for more than the ring holds gives what there is.
  std::array<TraceRecord, 8> all{};
  ASSERT_EQ(fixture.tracer.CopyLatestUnlocked(all), 4u);
  EXPECT_EQ(all[0].arg, 2u);
  EXPECT_EQ(all[3].arg, 5u);

  const TraceFixture<4> empty;
  EXPECT_EQ(empty.tracer.CopyLatestUnlocked(all), 0u);
}

TEST(Tracer, CopyLatestStillSeesRecordsThatWereAlreadyDrained) {
  TraceFixture<4> fixture;
  fixture.tracer.RecordInstant(IdOf(1), 1);
  fixture.tracer.RecordInstant(IdOf(2), 2);
  std::array<TraceRecord, 4> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 2u);

  std::array<TraceRecord, 4> latest{};
  ASSERT_EQ(fixture.tracer.CopyLatest(latest), 2u);
  EXPECT_EQ(latest[0].arg, 1u);
  EXPECT_EQ(latest[1].arg, 2u);
}

TEST(Tracer, EveryRingAccessHoldsTheLockAndReleasesIt) {
  TraceFixture<4> fixture;
  fixture.tracer.RecordInstant(kSpanMark, 0);
  EXPECT_EQ(fixture.sync.lockCount, 1);

  std::array<TraceRecord, 4> out{};
  EXPECT_EQ(fixture.tracer.Drain(out), 1u);
  EXPECT_EQ(fixture.tracer.CopyLatest(out), 1u);
  EXPECT_EQ(fixture.tracer.WrittenCount(), 1u);
  EXPECT_EQ(fixture.tracer.DroppedCount(), 0u);

  EXPECT_EQ(fixture.sync.lockCount, 5);
  EXPECT_EQ(fixture.sync.depth, 0);
  EXPECT_EQ(fixture.sync.maxDepth, 1);
}

TEST(Tracer, WithNoStorageRecordsAreDiscardedSafely) {
  bn::test::FakeClock clock;
  bn::test::FakeSync sync;
  bn::core::Tracer tracer({}, clock, sync);
  tracer.RecordInstant(kSpanMark, 1);
  tracer.RecordSpan(kSpanOuter, 0, 0);

  std::array<TraceRecord, 2> out{};
  EXPECT_EQ(tracer.Capacity(), 0u);
  EXPECT_EQ(tracer.Drain(out), 0u);
  EXPECT_EQ(tracer.CopyLatest(out), 0u);
  EXPECT_EQ(tracer.WrittenCount(), 0u);
}

TEST(Tracer, ASpanCanEndSomewhereOtherThanWhereItBegan) {
  TraceFixture<4> fixture;
  fixture.clock.now = 1'000;
  const std::uint32_t began = BN_TRACE_NOW();
  EXPECT_EQ(began, 1'000u);

  // Later, and from another context: an interrupt handler, say.
  fixture.clock.now = 1'450;
  fixture.sync.task = bn::core::kTraceTaskInterrupt;
  BN_TRACE_TOP_SPAN_SINCE(kSpanOuter, began, 7);

  std::array<TraceRecord, 4> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 1u);
  EXPECT_EQ(out[0].id, kSpanOuter);
  EXPECT_EQ(out[0].kind, bn::core::TraceKind::kSpan);
  EXPECT_EQ(out[0].startUs, 1'000u);
  EXPECT_EQ(out[0].durationUs, 450u);
  EXPECT_EQ(out[0].arg, 7u);
  EXPECT_EQ(out[0].task, bn::core::kTraceTaskInterrupt);
}

TEST(Tracer, EndingASpanWithNoTracerInstalledDoesNothing) {
  ASSERT_EQ(bn::core::ActiveTracer(), nullptr);
  EXPECT_EQ(BN_TRACE_NOW(), 0u);
  BN_TRACE_TOP_SPAN_SINCE(kSpanOuter, 123, 4);
}

}  // namespace
