#include "common/clock.hpp"
#include "server/pool/account_pool.hpp"
#include "server/pool/affinity.hpp"
#include "server/pool/health.hpp"
#include "support/fake_clock.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using qoderz::error_kind;
using qoderz::server::account_health;
using qoderz::server::account_pool;
using qoderz::server::failure_kind;
using qoderz::server::pool_account;
using qoderz::server::pool_options;
using qoderz::server::session_affinity;
using qoderz::test::fake_clock;

constexpr std::string_view site_id = "qoder-cn";
constexpr std::int64_t minute_ms = 60 * 1000;

pool_account make_account(std::string_view name, std::string_view site = site_id) {
  pool_account account{};
  account.m_key = std::string{site} + "/" + std::string{name};
  account.m_account.m_site = std::string{site};
  account.m_account.m_name = std::string{name};
  account.m_site.m_site_id = std::string{site};
  account.m_site.m_api_base = "https://api.example.invalid/" + std::string{name};
  return account;
}

std::vector<pool_account> three_accounts() {
  return {make_account("alpha"), make_account("bravo"), make_account("charlie")};
}

std::vector<std::string> keys_of(std::vector<pool_account const*> const& accounts) {
  std::vector<std::string> keys;
  keys.reserve(accounts.size());
  for (auto const* account : accounts) {
    keys.push_back(account->m_key);
  }
  return keys;
}

struct fixtures {
  fake_clock m_time;
  account_health m_health{m_time};
  session_affinity m_affinity;
};

qoderz::result<account_pool> make_pool(fixtures& scope, std::vector<pool_account> accounts,
                                       pool_options options = {}) {
  return account_pool::create(std::move(accounts), scope.m_health, scope.m_affinity,
                              std::move(options));
}

} // namespace

TEST_CASE("New sessions start at different accounts", "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  CHECK(accounts.size() == 3);
  CHECK_FALSE(accounts.empty());
  CHECK(accounts.site_id() == site_id);

  auto const first = keys_of(accounts.candidates("session-1", true));
  auto const second = keys_of(accounts.candidates("session-2", true));
  auto const third = keys_of(accounts.candidates("session-3", true));
  auto const fourth = keys_of(accounts.candidates("session-4", true));

  REQUIRE(first.size() == 3);
  CHECK(first == std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/bravo", "qoder-cn/charlie"});
  CHECK(second == std::vector<std::string>{"qoder-cn/bravo", "qoder-cn/charlie", "qoder-cn/alpha"});
  CHECK(third == std::vector<std::string>{"qoder-cn/charlie", "qoder-cn/alpha", "qoder-cn/bravo"});
  CHECK(fourth == first);
}

TEST_CASE("Accounts are ordered by name ignoring case", "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, {make_account("Bravo"), make_account("alpha"),
                                   make_account("charlie"), make_account("Delta")});
  REQUIRE(created.has_value());

  CHECK(keys_of(created->candidates("s", false)) ==
        std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/Bravo", "qoder-cn/charlie",
                                 "qoder-cn/Delta"});
}

TEST_CASE("A rotated candidate list keeps the bound account first",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  accounts.bind("session-1", "qoder-cn/charlie");
  CHECK(scope.m_affinity.bound("session-1", scope.m_time.now_ms()) ==
        std::optional<std::string>{"qoder-cn/charlie"});

  // With three accounts and `charlie` pinned to the front, the tail is always the same pair in the
  // rotation order; asserting only `picked[1] != picked[2]` would hold for any two-element tail.
  std::vector<std::vector<std::string>> tails;
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto const picked = keys_of(accounts.candidates("session-1", true));
    REQUIRE(picked.size() == 3);
    CHECK(picked.front() == "qoder-cn/charlie");
    auto const tail = std::vector<std::string>{picked[1], picked[2]};
    CHECK(std::find(tail.begin(), tail.end(), "qoder-cn/alpha") != tail.end());
    CHECK(std::find(tail.begin(), tail.end(), "qoder-cn/bravo") != tail.end());
    tails.push_back(tail);
  }
  // The rotation counter still advances under a bound session, so the tail order changes.
  CHECK(tails[0] != tails[1]);

  auto const plain = keys_of(accounts.candidates("session-2", true));
  REQUIRE(plain.size() == 3);
  CHECK(plain.front() != "qoder-cn/charlie");

  auto const other = keys_of(accounts.candidates("session-2", false));
  CHECK(other.front() != "qoder-cn/charlie");
}

TEST_CASE("A cooling bound account is skipped but still remembered",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  accounts.bind("session-1", "qoder-cn/alpha");
  accounts.report_failure("qoder-cn/alpha", failure_kind::quota);
  CHECK(accounts.cooling_count() == 1);

  auto const cooling = keys_of(accounts.candidates("session-1", true));
  REQUIRE(cooling.size() == 2);
  CHECK(std::find(cooling.begin(), cooling.end(), "qoder-cn/alpha") == cooling.end());

  accounts.report_success("qoder-cn/alpha");
  auto const recovered = keys_of(accounts.candidates("session-1", true));
  REQUIRE(recovered.size() == 3);
  CHECK(recovered.front() == "qoder-cn/alpha");
}

TEST_CASE("A binding to an account outside the pool counts as no binding",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  // The account a session was bound to was removed from the snapshot (or the snapshot was rebuilt
  // after `qoderz remove`): the binding survives in the table but names nothing this pool rotates
  // over, so the session falls back to the rotation order.
  accounts.bind("session-1", "qoder-cn/removed");
  CHECK(scope.m_affinity.bound("session-1", scope.m_time.now_ms()) ==
        std::optional<std::string>{"qoder-cn/removed"});

  auto const picked = keys_of(accounts.candidates("session-1", true));
  REQUIRE(picked.size() == 3);
  CHECK(picked == std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/bravo", "qoder-cn/charlie"});

  // The next successful open rebinds the session, and from then on the pool honours it.
  accounts.bind("session-1", "qoder-cn/bravo");
  CHECK(keys_of(accounts.candidates("session-1", true)).front() == "qoder-cn/bravo");
}

TEST_CASE("A bound session keeps its account while the rotation advances",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  accounts.bind("session-1", "qoder-cn/alpha");
  auto const other = keys_of(accounts.candidates("session-2", true));

  // The second session's call advanced the rotation counter, but session-1 still starts at its
  // binding, and the rest of its list stays a rotation of the healthy accounts.
  auto const bound = keys_of(accounts.candidates("session-1", true));
  REQUIRE(bound.size() == 3);
  CHECK(bound.front() == "qoder-cn/alpha");
  CHECK(std::find(bound.begin() + 1, bound.end(), "qoder-cn/bravo") != bound.end());
  CHECK(std::find(bound.begin() + 1, bound.end(), "qoder-cn/charlie") != bound.end());
  CHECK(other.size() == 3);
}

TEST_CASE("Models style candidates do not touch the binding or the rotation",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  auto const first = keys_of(accounts.candidates("session-1", false));
  auto const second = keys_of(accounts.candidates("session-2", false));
  CHECK(first == second);
  CHECK(scope.m_affinity.size() == 0);

  // A healthy binding must not reorder a models-style list either: `rotate = false` never reads it.
  accounts.bind("session-1", "qoder-cn/charlie");
  CHECK(keys_of(accounts.candidates("session-1", false)) == first);
  CHECK(keys_of(accounts.candidates("session-1", false)).front() != "qoder-cn/charlie");

  static_cast<void>(accounts.candidates("session-1", false));
  static_cast<void>(accounts.candidates("session-1", false));
  auto const rotated = keys_of(accounts.candidates("session-9", true));
  CHECK(rotated ==
        std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/bravo", "qoder-cn/charlie"});
}

TEST_CASE("A pinned pool produces only its account", "[server][pool][account_pool]") {
  fixtures scope;
  pool_options options{};
  options.m_pinned_key = "qoder-cn/bravo";
  auto created = make_pool(scope, three_accounts(), options);
  REQUIRE(created.has_value());
  auto& accounts = *created;

  CHECK(keys_of(accounts.candidates("session-1", true)) ==
        std::vector<std::string>{"qoder-cn/bravo"});
  CHECK(accounts.keys() == std::vector<std::string>{"qoder-cn/bravo"});
  CHECK(accounts.size() == 1);
}

TEST_CASE("A pinned pool honors cooldowns unless they are ignored",
          "[server][pool][account_pool]") {
  fixtures scope;
  pool_options options{};
  options.m_pinned_key = "qoder-cn/bravo";
  auto created = make_pool(scope, three_accounts(), options);
  REQUIRE(created.has_value());
  auto& accounts = *created;

  accounts.report_failure("qoder-cn/bravo", failure_kind::auth);
  CHECK(accounts.candidates("session-1", true).empty());
  CHECK(accounts.no_candidates_error().m_kind == error_kind::auth);
  CHECK(accounts.no_candidates_error().m_message ==
        "all Qoder accounts are temporarily unavailable; "
        "retry later");

  fixtures ignoring_scope;
  pool_options ignoring{};
  ignoring.m_pinned_key = "qoder-cn/bravo";
  ignoring.m_ignore_cooldown = true;
  auto ignoring_pool = make_pool(ignoring_scope, three_accounts(), ignoring);
  REQUIRE(ignoring_pool.has_value());
  auto& ignoring_accounts = *ignoring_pool;

  ignoring_accounts.report_failure("qoder-cn/bravo", failure_kind::auth);
  CHECK(keys_of(ignoring_accounts.candidates("session-1", true)) ==
        std::vector<std::string>{"qoder-cn/bravo"});
  CHECK(ignoring_accounts.cooling_count() == 1);

  // A quota cooldown on the pinned account is a 429, not a 401, and the pinned pool never looks at
  // the cooldown of an account outside its one-element rotation.
  accounts.report_success("qoder-cn/bravo");
  accounts.report_failure("qoder-cn/bravo", failure_kind::quota);
  accounts.report_failure("qoder-cn/charlie", failure_kind::auth);
  auto const quota = accounts.no_candidates_error();
  CHECK(quota.m_kind == error_kind::quota);
  CHECK(quota.m_http_status == 429);
  CHECK(quota.m_message == "all Qoder accounts are temporarily unavailable; retry later");
}

TEST_CASE("A subset pool rotates only inside the subset", "[server][pool][account_pool]") {
  fixtures scope;
  pool_options options{};
  options.m_only_keys = {"qoder-cn/charlie", "qoder-cn/alpha"};
  auto created = make_pool(scope, three_accounts(), options);
  REQUIRE(created.has_value());
  auto& accounts = *created;

  auto const first = keys_of(accounts.candidates("session-1", false));
  auto const second = keys_of(accounts.candidates("session-2", false));
  CHECK(first == std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/charlie"});
  CHECK(second == first);

  auto const rotated = keys_of(accounts.candidates("session-3", true));
  auto const rotated_again = keys_of(accounts.candidates("session-4", true));
  CHECK(rotated == std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/charlie"});
  CHECK(rotated_again == std::vector<std::string>{"qoder-cn/charlie", "qoder-cn/alpha"});
  CHECK(accounts.size() == 2);
  CHECK_FALSE(accounts.empty());
  CHECK(accounts.keys() == std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/charlie"});
}

TEST_CASE("An empty pool is not signed in", "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, {});
  REQUIRE(created.has_value());
  auto& accounts = *created;

  CHECK(accounts.empty());
  CHECK(accounts.size() == 0);
  CHECK(accounts.site_id().empty());
  CHECK(accounts.candidates("session-1", true).empty());

  auto const no_account = accounts.no_candidates_error();
  CHECK(no_account.m_kind == error_kind::auth);
  CHECK(no_account.m_http_status == 401);
  CHECK(no_account.m_message == "not signed in; run: qoderz login");

  // An empty pool has nothing to bind and nothing to cool down; binding must not grow the table and
  // reporting must not invent an account.
  accounts.bind("session-1", "qoder-cn/ghost");
  CHECK(scope.m_affinity.size() == 1);
  accounts.report_failure("qoder-cn/ghost", failure_kind::auth);
  CHECK(accounts.cooling_count() == 0);
  CHECK(accounts.no_candidates_error().m_message == "not signed in; run: qoderz login");
}

TEST_CASE("A fully cooled pool reports the cooldown kind", "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  for (auto const& account : three_accounts()) {
    accounts.report_failure(account.m_key, failure_kind::auth);
  }

  CHECK(accounts.candidates("session-1", true).empty());
  CHECK(accounts.cooling_count() == 3);

  auto const auth_only = accounts.no_candidates_error();
  CHECK(auth_only.m_kind == error_kind::auth);
  CHECK(auth_only.m_http_status == 401);
  CHECK(auth_only.m_message == "all Qoder accounts are temporarily unavailable; retry later");

  accounts.report_failure("qoder-cn/bravo", failure_kind::quota);
  auto const mixed = accounts.no_candidates_error();
  CHECK(mixed.m_kind == error_kind::quota);
  CHECK(mixed.m_http_status == 429);
  CHECK(mixed.m_message == "all Qoder accounts are temporarily unavailable; retry later");

  // A single account is the smallest pool that can be fully cooled; quota alone must answer 429.
  fixtures single_scope;
  auto single = make_pool(single_scope, {make_account("alpha")});
  REQUIRE(single.has_value());
  single->report_failure("qoder-cn/alpha", failure_kind::quota);
  auto const single_error = single->no_candidates_error();
  CHECK(single_error.m_kind == error_kind::quota);
  CHECK(single_error.m_http_status == 429);

  // The cooldown expiring makes the same pool serve again instead of failing.
  single_scope.m_time.advance(std::chrono::milliseconds{5 * minute_ms});
  CHECK(single->candidates("session-1", true).size() == 1);
}

TEST_CASE("A partly cooled pool still serves from the healthy accounts",
          "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  accounts.report_failure("qoder-cn/alpha", failure_kind::quota);
  auto const picked = keys_of(accounts.candidates("session-1", false));
  CHECK(picked == std::vector<std::string>{"qoder-cn/bravo", "qoder-cn/charlie"});

  scope.m_time.advance(std::chrono::milliseconds{5 * minute_ms});
  CHECK(keys_of(accounts.candidates("session-1", false)).size() == 3);
  CHECK(accounts.cooling_count() == 0);
}

TEST_CASE("Building a pool rejects inconsistent input", "[server][pool][account_pool]") {
  fixtures scope;

  SECTION("accounts of two sites") {
    auto created = make_pool(scope, {make_account("alpha"), make_account("beta", "qoder")});
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().m_kind == error_kind::configuration);
    CHECK(created.error().m_message.find("one site") != std::string::npos);
  }

  SECTION("a pinned account that does not exist") {
    pool_options options{};
    options.m_pinned_key = "qoder-cn/missing";
    auto created = make_pool(scope, three_accounts(), options);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().m_kind == error_kind::configuration);
    CHECK(created.error().m_message.find("qoder-cn/missing") != std::string::npos);
  }

  SECTION("a subset account that does not exist") {
    pool_options options{};
    options.m_only_keys = {"qoder-cn/alpha", "qoder-cn/missing"};
    auto created = make_pool(scope, three_accounts(), options);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().m_kind == error_kind::configuration);
  }

  SECTION("a pinned account plus a subset") {
    pool_options options{};
    options.m_pinned_key = "qoder-cn/alpha";
    options.m_only_keys = {"qoder-cn/alpha"};
    auto created = make_pool(scope, three_accounts(), options);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().m_kind == error_kind::configuration);
  }

  SECTION("a duplicate key") {
    auto created = make_pool(scope, {make_account("alpha"), make_account("alpha")});
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().m_kind == error_kind::configuration);
  }
}

TEST_CASE("One clock and one binding table serve the pool", "[server][pool][account_pool]") {
  fake_clock time;
  account_health health{time};
  session_affinity affinity{};
  auto created = account_pool::create(three_accounts(), health, affinity);
  REQUIRE(created.has_value());
  auto& accounts = *created;

  CHECK(accounts.size() == 3);

  accounts.bind("session-1", "qoder-cn/charlie");
  auto const bound = affinity.bound("session-1", time.now_ms());
  REQUIRE(bound.has_value());
  CHECK(*bound == "qoder-cn/charlie");

  accounts.report_failure("qoder-cn/bravo", failure_kind::quota);
  CHECK(health.is_cooling("qoder-cn/bravo"));
  CHECK(accounts.cooling_count() == 1);

  time.advance(std::chrono::milliseconds{5 * minute_ms});
  CHECK_FALSE(health.is_cooling("qoder-cn/bravo"));

  pool_options options{};
  options.m_pinned_key = "qoder-cn/missing";
  CHECK_FALSE(account_pool::create(three_accounts(), health, affinity, options).has_value());
}

TEST_CASE("Concurrent rotation and binding stays consistent", "[server][pool][account_pool]") {
  fixtures scope;
  auto created = make_pool(scope, three_accounts());
  REQUIRE(created.has_value());
  auto& accounts = *created;

  // 8 workers x 40 sessions = 320 bindings, well below the 512-entry capacity, and the clock never
  // advances, so no binding is evicted behind a worker's back. Each worker records what it saw and
  // the assertions run after the join: Catch2 assertions are not safe to call from several threads.
  std::vector<std::string> bad_shape;
  std::vector<std::string> bad_rotation;
  std::vector<std::string> bad_affinity;
  std::mutex report_mutex;

  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&, worker] {
      for (std::size_t index = 0; index < 40; ++index) {
        auto const name = "session-" + std::to_string((worker * 100) + index);
        auto const picked = keys_of(accounts.candidates(name, true));

        // Every call returns the whole healthy set exactly once, whatever the rotation offset is.
        auto sorted = picked;
        std::sort(sorted.begin(), sorted.end());
        if (sorted !=
            std::vector<std::string>{"qoder-cn/alpha", "qoder-cn/bravo", "qoder-cn/charlie"}) {
          std::lock_guard<std::mutex> guard{report_mutex};
          bad_shape.push_back(name);
          continue;
        }

        accounts.bind(name, picked.front());
        auto const rebound = keys_of(accounts.candidates(name, true));
        std::lock_guard<std::mutex> guard{report_mutex};
        if (rebound.size() != 3) {
          bad_rotation.push_back(name);
        } else if (rebound.front() != picked.front()) {
          bad_affinity.push_back(name);
        }
      }
    });
  }
  workers.clear();

  CHECK(bad_shape.empty());
  CHECK(bad_rotation.empty());
  CHECK(bad_affinity.empty());
  CHECK(accounts.size() == 3);
  CHECK(scope.m_affinity.size() == 320);
}
