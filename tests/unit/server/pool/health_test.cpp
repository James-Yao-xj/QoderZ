#include "server/pool/health.hpp"
#include "support/fake_clock.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using qoderz::server::account_health;
using qoderz::server::failure_kind;
using qoderz::test::fake_clock;

constexpr std::string_view work_key = "qoder-cn/work";
constexpr std::string_view other_key = "qoder-cn/home";
constexpr std::int64_t minute_ms = 60 * 1000;
constexpr std::int64_t two_hours_ms = 2 * 60 * minute_ms;

static_assert(account_health::cooldown_ms(failure_kind::quota, 0) == 5 * minute_ms);
static_assert(account_health::cooldown_ms(failure_kind::auth, 0) == 15 * minute_ms);
static_assert(account_health::cooldown_ms(failure_kind::quota, -3) == 5 * minute_ms);

std::int64_t remaining_ms(account_health const& health, std::string_view key, std::int64_t now_ms) {
  auto const recorded = health.state(key);
  REQUIRE(recorded.has_value());
  return recorded->m_cooldown_until_ms - now_ms;
}

} // namespace

TEST_CASE("Quota cooldowns double from five minutes and stop at one hour",
          "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  CHECK_FALSE(health.is_cooling(work_key));

  constexpr std::array<std::int64_t, 6> expected_minutes{5, 10, 20, 40, 60, 60};
  for (std::size_t index = 0; index < expected_minutes.size(); ++index) {
    time.advance(std::chrono::milliseconds{minute_ms});
    health.report_failure(work_key, failure_kind::quota);

    auto const recorded = health.state(work_key);
    REQUIRE(recorded.has_value());
    CHECK(recorded->m_kind == failure_kind::quota);
    CHECK(recorded->m_failures == static_cast<int>(index) + 1);
    CHECK(remaining_ms(health, work_key, time.now_ms()) == expected_minutes[index] * minute_ms);
    CHECK(health.is_cooling(work_key));
  }
}

TEST_CASE("Auth cooldowns double from fifteen minutes and stop at one hour",
          "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  constexpr std::array<std::int64_t, 4> expected_minutes{15, 30, 60, 60};
  for (std::size_t index = 0; index < expected_minutes.size(); ++index) {
    health.report_failure(work_key, failure_kind::auth);
    CHECK(remaining_ms(health, work_key, time.now_ms()) == expected_minutes[index] * minute_ms);
    time.advance(std::chrono::milliseconds{minute_ms});
  }
}

TEST_CASE("A different failure kind restarts the streak", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  health.report_failure(work_key, failure_kind::quota);
  health.report_failure(work_key, failure_kind::quota);
  CHECK(remaining_ms(health, work_key, time.now_ms()) == 10 * minute_ms);

  health.report_failure(work_key, failure_kind::auth);
  auto const recorded = health.state(work_key);
  REQUIRE(recorded.has_value());
  CHECK(recorded->m_kind == failure_kind::auth);
  CHECK(recorded->m_failures == 1);
  CHECK(remaining_ms(health, work_key, time.now_ms()) == 15 * minute_ms);
}

TEST_CASE("A success clears the entry and its streak", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  health.report_failure(work_key, failure_kind::quota);
  health.report_failure(work_key, failure_kind::quota);
  health.report_success(work_key);

  CHECK_FALSE(health.is_cooling(work_key));
  CHECK_FALSE(health.state(work_key).has_value());
  CHECK(health.cooling_count() == 0);

  health.report_failure(work_key, failure_kind::quota);
  auto const recorded = health.state(work_key);
  REQUIRE(recorded.has_value());
  CHECK(recorded->m_failures == 1);
  CHECK(remaining_ms(health, work_key, time.now_ms()) == 5 * minute_ms);
}

TEST_CASE("A finished cooldown keeps its streak for two hours", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  for (int attempt = 0; attempt < 3; ++attempt) {
    health.report_failure(work_key, failure_kind::quota);
  }
  CHECK(remaining_ms(health, work_key, time.now_ms()) == 20 * minute_ms);

  time.advance(std::chrono::milliseconds{20 * minute_ms});
  CHECK_FALSE(health.is_cooling(work_key));
  CHECK(health.cooling_count() == 0);

  auto const retained = health.state(work_key);
  REQUIRE(retained.has_value());
  CHECK(retained->m_failures == 3);

  health.report_failure(work_key, failure_kind::quota);
  auto const resumed = health.state(work_key);
  REQUIRE(resumed.has_value());
  CHECK(resumed->m_failures == 4);
  CHECK(remaining_ms(health, work_key, time.now_ms()) == 40 * minute_ms);
}

TEST_CASE("An entry is pruned two hours after its cooldown ended", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  health.report_failure(work_key, failure_kind::quota);
  health.report_failure(other_key, failure_kind::quota);

  time.advance(std::chrono::milliseconds{5 * minute_ms + two_hours_ms});
  health.prune();
  CHECK(health.state(work_key).has_value());
  CHECK(health.state(other_key).has_value());

  time.advance(std::chrono::milliseconds{1});
  health.prune();
  CHECK_FALSE(health.state(work_key).has_value());
  CHECK_FALSE(health.state(other_key).has_value());
}

TEST_CASE("A report prunes entries whose retention ran out", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  health.report_failure(work_key, failure_kind::quota);
  time.advance(std::chrono::milliseconds{5 * minute_ms + two_hours_ms + 1});

  health.report_success(other_key);
  CHECK_FALSE(health.state(work_key).has_value());
}

TEST_CASE("Counting cooling entries and their kinds", "[server][pool][health]") {
  fake_clock time;
  account_health health{time};

  CHECK(health.cooling_count() == 0);
  CHECK_FALSE(health.all_cooling_auth());

  health.report_failure(work_key, failure_kind::auth);
  CHECK(health.cooling_count() == 1);
  CHECK(health.all_cooling_auth());

  health.report_failure(other_key, failure_kind::quota);
  CHECK(health.cooling_count() == 2);
  CHECK_FALSE(health.all_cooling_auth());

  time.advance(std::chrono::milliseconds{6 * minute_ms});
  CHECK(health.cooling_count() == 1);
  CHECK(health.all_cooling_auth());

  time.advance(std::chrono::milliseconds{10 * minute_ms});
  CHECK(health.cooling_count() == 0);
  CHECK_FALSE(health.all_cooling_auth());
}

TEST_CASE("A clock that jumps backwards only lengthens a cooldown", "[server][pool][health]") {
  // Wall clocks can be corrected backwards. Nothing here may underflow: a cooldown measured against
  // a smaller `now` is simply longer, and no entry may be resurrected as healthy by the jump.
  fake_clock time;
  account_health health{time};

  health.report_failure(work_key, failure_kind::quota);
  auto const before = health.state(work_key);
  REQUIRE(before.has_value());
  CHECK(health.is_cooling(work_key));

  time.set_now_ms(time.now_ms() - 60 * minute_ms);
  CHECK(health.is_cooling(work_key));
  auto const after = health.state(work_key);
  REQUIRE(after.has_value());
  CHECK(after->m_cooldown_until_ms == before->m_cooldown_until_ms);
  CHECK(health.cooling_count() == 1);

  // Pruning compares `now > prune_after`, so a backwards jump never drops a retained entry either.
  health.prune();
  CHECK(health.state(work_key).has_value());
}

TEST_CASE("Failure kinds render as their documented names", "[server][pool][health]") {
  CHECK(qoderz::server::to_string(failure_kind::auth) == "auth");
  CHECK(qoderz::server::to_string(failure_kind::quota) == "quota");
  CHECK(static_cast<int>(failure_kind::auth) == 0);
  CHECK(static_cast<int>(failure_kind::quota) == 1);
}
