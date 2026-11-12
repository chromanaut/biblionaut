#include "reader/app.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

class FakeClock final : public bn::core::Clock {
 public:
  std::uint64_t NowUs() noexcept override { return now; }
  std::uint64_t now = 0;
};

TEST(App, UptimeIsZeroBeforeStart) {
  FakeClock clock;
  clock.now = 500;
  const bn::reader::App app(bn::reader::Services{clock});
  EXPECT_FALSE(app.Started());
  EXPECT_EQ(app.UptimeUs(), 0u);
}

TEST(App, UptimeCountsFromStart) {
  FakeClock clock;
  clock.now = 1'000;
  bn::reader::App app(bn::reader::Services{clock});
  app.Start();
  EXPECT_TRUE(app.Started());
  EXPECT_EQ(app.UptimeUs(), 0u);

  clock.now = 3'500;
  EXPECT_EQ(app.UptimeUs(), 2'500u);
}

TEST(App, DefaultServicesUseTheLinkedHardwarePort) {
  bn::reader::Services services = bn::reader::DefaultServices();
  const std::uint64_t first = services.clock.NowUs();
  const std::uint64_t second = services.clock.NowUs();
  EXPECT_GE(second, first);
}

TEST(Version, IsNotEmpty) { EXPECT_GT(std::strlen(bn::reader::Version()), 0u); }

}  // namespace
