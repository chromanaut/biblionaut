// This file is compiled as if the build had set the trace level to 0.
#undef BN_TRACE_LEVEL
#define BN_TRACE_LEVEL 0
#include <gtest/gtest.h>

#include "core/trace.h"
#include "trace_fakes.h"

namespace {

using bn::test::kSpanInner;
using bn::test::kSpanMark;
using bn::test::kSpanOuter;

TEST(TraceLevel0, EveryMacroCompilesToNothing) {
  bn::test::TraceFixture<8> fixture;
  int evaluations = 0;
  {
    BN_TRACE_TOP_SCOPE(kSpanOuter);
    BN_TRACE_TOP_SCOPE_ARG(kSpanOuter, ++evaluations);
    BN_TRACE_SCOPE(kSpanInner);
    BN_TRACE_SCOPE_ARG(kSpanInner, ++evaluations);
    BN_TRACE_INSTANT(kSpanMark, ++evaluations);
  }
  fixture.clock.now = 500;
  const std::uint32_t began = BN_TRACE_NOW();
  EXPECT_EQ(began, 0u);  // The clock is not even read.
  BN_TRACE_TOP_SPAN_SINCE(kSpanOuter, began + static_cast<std::uint32_t>(++evaluations), ++evaluations);
  EXPECT_EQ(fixture.tracer.WrittenCount(), 0u);
  EXPECT_EQ(evaluations, 0);
  EXPECT_EQ(fixture.sync.lockCount, 1);  // Only the WrittenCount() call above.
}

}  // namespace
