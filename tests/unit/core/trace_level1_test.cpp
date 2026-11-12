// This file is compiled as if the build had set the trace level to 1.
#undef BN_TRACE_LEVEL
#define BN_TRACE_LEVEL 1
#include <gtest/gtest.h>

#include <array>

#include "core/trace.h"
#include "trace_fakes.h"

namespace {

using bn::core::TraceRecord;
using bn::test::kSpanInner;
using bn::test::kSpanMark;
using bn::test::kSpanOuter;

TEST(TraceLevel1, KeepsTopLevelSpansAndInstantsOnly) {
  bn::test::TraceFixture<8> fixture;
  int evaluations = 0;
  {
    BN_TRACE_TOP_SCOPE_ARG(kSpanOuter, 5);
    BN_TRACE_SCOPE(kSpanInner);
    BN_TRACE_SCOPE_ARG(kSpanInner, ++evaluations);
    BN_TRACE_INSTANT(kSpanMark, 6);
  }

  std::array<TraceRecord, 8> out{};
  ASSERT_EQ(fixture.tracer.Drain(out), 2u);
  EXPECT_EQ(out[0].id, kSpanMark);
  EXPECT_EQ(out[0].arg, 6u);
  EXPECT_EQ(out[1].id, kSpanOuter);
  EXPECT_EQ(out[1].arg, 5u);
  EXPECT_EQ(evaluations, 0);
}

}  // namespace
