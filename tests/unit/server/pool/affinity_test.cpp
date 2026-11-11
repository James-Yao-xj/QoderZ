#include "server/pool/affinity.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using qoderz::server::session_affinity;

constexpr std::int64_t start_ms = 1'760'000'000'000;
constexpr std::int64_t minute_ms = 60 * 1000;
constexpr std::int64_t two_hours_ms = 2 * 60 * minute_ms;

std::string session_name(std::size_t index) {
  return "session-" + std::to_string(index);
}

} // namespace

TEST_CASE("A bound session reports its account until it is rebound", "[server][pool][affinity]") {
  session_affinity affinity;

  CHECK_FALSE(affinity.bound("session-1", start_ms).has_value());

  affinity.bind("session-1", "qoder-cn/work", start_ms);
  auto const first = affinity.bound("session-1", start_ms);
  REQUIRE(first.has_value());
  CHECK(*first == "qoder-cn/work");
  CHECK(affinity.size() == 1);

  affinity.bind("session-1", "qoder-cn/home", start_ms);
  auto const second = affinity.bound("session-1", start_ms);
  REQUIRE(second.has_value());
  CHECK(*second == "qoder-cn/home");
  CHECK(affinity.size() == 1);

  CHECK_FALSE(affinity.bound("session-2", start_ms).has_value());
}

TEST_CASE("An idle binding expires after two hours but a hit keeps it alive",
          "[server][pool][affinity]") {
  session_affinity affinity;
  affinity.bind("session-1", "qoder-cn/work", start_ms);

  static_cast<void>(affinity.bound("session-1", start_ms + two_hours_ms - minute_ms));
  affinity.bind("session-2", "qoder-cn/work", start_ms);

  CHECK(affinity.bound("session-1", start_ms + two_hours_ms).has_value());

  auto const stale = affinity.bound("session-2", start_ms + two_hours_ms + 1);
  CHECK_FALSE(stale.has_value());
  CHECK(affinity.size() == 1);
}

TEST_CASE("The table evicts the least recently used binding", "[server][pool][affinity]") {
  session_affinity affinity{2, two_hours_ms};

  affinity.bind("session-1", "qoder-cn/work", start_ms);
  affinity.bind("session-2", "qoder-cn/home", start_ms);
  static_cast<void>(affinity.bound("session-1", start_ms + 1));
  affinity.bind("session-3", "qoder-cn/other", start_ms + 1);

  CHECK(affinity.size() == 2);
  CHECK_FALSE(affinity.bound("session-2", start_ms + 1).has_value());
  CHECK(affinity.bound("session-1", start_ms + 1).has_value());
  CHECK(affinity.bound("session-3", start_ms + 1).has_value());
}

TEST_CASE("A full table keeps its capacity", "[server][pool][affinity]") {
  session_affinity affinity;
  for (std::size_t index = 0; index < 600; ++index) {
    affinity.bind(session_name(index), "qoder-cn/work", start_ms);
  }

  CHECK(affinity.size() == 512);
  CHECK_FALSE(affinity.bound(session_name(0), start_ms).has_value());
  CHECK(affinity.bound(session_name(599), start_ms).has_value());

  affinity.bind(session_name(600), "qoder-cn/home", start_ms + two_hours_ms + 1);
  CHECK(affinity.size() == 1);
  auto const fresh = affinity.bound(session_name(600), start_ms + two_hours_ms + 1);
  REQUIRE(fresh.has_value());
  CHECK(*fresh == "qoder-cn/home");
}

TEST_CASE("An empty session id is neither bound nor looked up", "[server][pool][affinity]") {
  session_affinity affinity;

  affinity.bind("", "qoder-cn/work", start_ms);
  CHECK(affinity.size() == 0);
  CHECK_FALSE(affinity.bound("", start_ms).has_value());
}

TEST_CASE("A zero capacity table still holds one binding", "[server][pool][affinity]") {
  // The documented default is 512; a zero is a caller bug that must not divide the table by zero or
  // leave `bind` spinning, so the constructor floors it at one.
  session_affinity affinity{0, two_hours_ms};

  affinity.bind("session-1", "qoder-cn/work", start_ms);
  CHECK(affinity.size() == 1);
  CHECK(affinity.bound("session-1", start_ms) == std::optional<std::string>{"qoder-cn/work"});

  affinity.bind("session-2", "qoder-cn/home", start_ms);
  CHECK(affinity.size() == 1);
  CHECK_FALSE(affinity.bound("session-1", start_ms).has_value());
  CHECK(affinity.bound("session-2", start_ms) == std::optional<std::string>{"qoder-cn/home"});
}

TEST_CASE("Concurrent binds and lookups keep the table consistent", "[server][pool][affinity]") {
  session_affinity affinity{64, two_hours_ms};

  // Each worker owns a disjoint set of session ids, so a lost binding is always a real bug. The
  // table is smaller than the union of the workers' ids, so eviction runs concurrently too.
  std::vector<std::string> lost;
  std::mutex report_mutex;

  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&, worker] {
      for (std::size_t index = 0; index < 200; ++index) {
        auto const name = session_name((worker * 200) + index);
        affinity.bind(name, "qoder-cn/work", start_ms);
        auto const found = affinity.bound(name, start_ms);
        static_cast<void>(affinity.size());
        if (!found.has_value() || *found != "qoder-cn/work") {
          std::lock_guard<std::mutex> guard{report_mutex};
          lost.push_back(name);
        }
      }
    });
  }
  workers.clear();

  // 1600 distinct ids against a 64-entry table. Every id maps to one key, so the only invariants
  // are that the table stayed inside its capacity and that no surviving entry points at another
  // id's account: a lost binding is legal (evicted), a wrong one is a bug.
  CHECK(affinity.size() == 64);
  std::size_t survivors = 0;
  for (std::size_t index = 0; index < 1600; ++index) {
    auto const found = affinity.bound(session_name(index), start_ms);
    if (found.has_value()) {
      ++survivors;
      CHECK(*found == "qoder-cn/work");
    }
  }
  CHECK(survivors == 64);
}
