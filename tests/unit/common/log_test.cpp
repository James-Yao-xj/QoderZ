#include "common/log.hpp"
#include "unit/qoder/stderr_capture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

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

TEST_CASE("log values escape control characters", "[common][log]") {
  std::string value{"line\nbreak\rcarriage"};
  value.push_back('\t');
  value += "tab";
  value.push_back('\x01');
  value += "unit";

  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  qoderz::log(log_level::info, "control characters", {{"value", value}});
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  CHECK(logged.find(R"(value="line\nbreak\rcarriage\ttab\x01unit")") != std::string::npos);
  CHECK(logged.find('\r') == std::string::npos);
  CHECK(logged.find('\t') == std::string::npos);
  CHECK(logged.find('\x01') == std::string::npos);
  REQUIRE(logged.find('\n') != std::string::npos);
  CHECK(logged.find('\n') == logged.size() - 1);
  CHECK(std::count(logged.begin(), logged.end(), '\n') == 1);
}

TEST_CASE("a log value cannot forge a second record", "[common][log]") {
  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  qoderz::log(log_level::info, "forged record attempt", {{"reason", "first\nlevel=error forged"}});
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  CHECK(std::count(logged.begin(), logged.end(), '\n') == 1);
  CHECK(logged.find("\nlevel=error") == std::string::npos);
  CHECK(logged.find(R"(reason="first\nlevel=error forged")") != std::string::npos);
}
