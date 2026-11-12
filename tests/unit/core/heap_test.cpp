#include "core/heap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

namespace {

using bn::core::HeapFailure;

// Volatile so the compiler cannot see the size and reject or fold the call.
volatile std::size_t impossibleSize = std::numeric_limits<std::size_t>::max() / 2;

HeapFailure lastFailure{};
int handlerCalls = 0;

void RecordFailure(const HeapFailure& failure) noexcept {
  lastFailure = failure;
  ++handlerCalls;
}

class Heap : public testing::Test {
 protected:
  void SetUp() override {
    handlerCalls = 0;
    lastFailure = {};
    previous_ = bn::core::SetHeapFailureHandler(RecordFailure);
  }
  void TearDown() override { bn::core::SetHeapFailureHandler(previous_); }

 private:
  bn::core::HeapFailureHandler previous_ = nullptr;
};

TEST_F(Heap, NewAndDeleteRoundTrip) {
  int* value = new int(42);
  EXPECT_EQ(*value, 42);
  delete value;

  const std::unique_ptr<char[]> bytes(new (std::nothrow) char[128]);
  ASSERT_NE(bytes, nullptr);
  bytes[127] = 1;

  EXPECT_EQ(handlerCalls, 0);
}

TEST_F(Heap, OverAlignedNewIsAligned) {
  struct alignas(64) Wide {
    char data[64];
  };
  const std::unique_ptr<Wide> wide(new (std::nothrow) Wide);
  ASSERT_NE(wide, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(wide.get()) % 64, 0u);
}

TEST_F(Heap, NothrowNewReturnsNullAndReportsTheFailure) {
  const std::uint32_t before = bn::core::HeapFailureCount();

  char* bytes = new (std::nothrow) char[impossibleSize];

  EXPECT_EQ(bytes, nullptr);
  EXPECT_EQ(handlerCalls, 1);
  EXPECT_EQ(lastFailure.size, impossibleSize);
  EXPECT_FALSE(lastFailure.fatal);
  EXPECT_EQ(bn::core::HeapFailureCount(), before + 1);
}

TEST_F(Heap, SetHandlerReturnsThePreviousOne) {
  EXPECT_EQ(bn::core::SetHeapFailureHandler(nullptr), RecordFailure);
  EXPECT_EQ(bn::core::SetHeapFailureHandler(RecordFailure), nullptr);
}

using HeapDeathTest = Heap;

TEST_F(HeapDeathTest, PlainNewReportsThenAborts) {
  EXPECT_DEATH(
      {
        bn::core::SetHeapFailureHandler(
            [](const HeapFailure& failure) noexcept { std::fprintf(stderr, "fatal=%d\n", failure.fatal ? 1 : 0); });
        // Called directly: unlike a new-expression, the optimiser may not
        // remove a call to the allocation function.
        void* bytes = ::operator new(impossibleSize);
        ::operator delete(bytes);
      },
      "fatal=1");
}

}  // namespace
