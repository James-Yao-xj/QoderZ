#include "common/log.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>

using qoderz::log_level;
static_assert(qoderz::to_string(log_level::warn) == "warn");
static_assert(qoderz::parse_log_level("INFO") == log_level::info);
TEST_CASE("log level names round-trip", "[common][log]") {
  constexpr std::array levels{log_level::trace, log_level::debug, log_level::info,
                              log_level::warn,  log_level::error, log_level::critical};
  for (auto const level : levels) {
    CHECK(qoderz::parse_log_level(qoderz::to_string(level)) == level);
  }
}

TEST_CASE("log level parsing ignores case", "[common][log]") {
  CHECK(qoderz::parse_log_level("TRACE") == log_level::trace);
  CHECK(qoderz::parse_log_level("Debug") == log_level::debug);
  CHECK(qoderz::parse_log_level("INFO") == log_level::info);
  CHECK(qoderz::parse_log_level("Warn") == log_level::warn);
  CHECK(qoderz::parse_log_level("ERROR") == log_level::error);
  CHECK(qoderz::parse_log_level("Critical") == log_level::critical);
}

TEST_CASE("unknown log levels are rejected", "[common][log]") {
  CHECK_FALSE(qoderz::parse_log_level("").has_value());
  CHECK_FALSE(qoderz::parse_log_level("warning").has_value());
  CHECK_FALSE(qoderz::parse_log_level(" info").has_value());
  CHECK_FALSE(qoderz::parse_log_level("verbose").has_value());
}

TEST_CASE("log level can be set and queried", "[common][log]") {
  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(log_level::trace);
  CHECK(qoderz::current_log_level() == log_level::trace);
  qoderz::set_log_level(previous);
  CHECK(qoderz::current_log_level() == previous);
}
