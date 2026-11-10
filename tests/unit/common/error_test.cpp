#include "common/error.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <string>

using qoderz::error_kind;
using qoderz::result;
static_assert(qoderz::to_string(error_kind::quota) == "quota");
static_assert(qoderz::default_http_status(error_kind::quota) == 429);
TEST_CASE("error kinds have stable names", "[common][error]") {
  CHECK(qoderz::to_string(error_kind::configuration) == "configuration");
  CHECK(qoderz::to_string(error_kind::auth) == "auth");
  CHECK(qoderz::to_string(error_kind::quota) == "quota");
  CHECK(qoderz::to_string(error_kind::busy) == "busy");
  CHECK(qoderz::to_string(error_kind::bad_request) == "bad_request");
  CHECK(qoderz::to_string(error_kind::upstream) == "upstream");
  CHECK(qoderz::to_string(error_kind::io) == "io");
  CHECK(qoderz::to_string(error_kind::protocol) == "protocol");
  CHECK(qoderz::to_string(error_kind::canceled) == "canceled");
  CHECK(qoderz::to_string(error_kind::internal) == "internal");
}

TEST_CASE("error kinds map to default HTTP statuses", "[common][error]") {
  CHECK(qoderz::default_http_status(error_kind::configuration) == 500);
  CHECK(qoderz::default_http_status(error_kind::auth) == 401);
  CHECK(qoderz::default_http_status(error_kind::quota) == 429);
  CHECK(qoderz::default_http_status(error_kind::busy) == 503);
  CHECK(qoderz::default_http_status(error_kind::bad_request) == 400);
  CHECK(qoderz::default_http_status(error_kind::upstream) == 502);
  CHECK(qoderz::default_http_status(error_kind::io) == 500);
  CHECK(qoderz::default_http_status(error_kind::protocol) == 502);
  CHECK(qoderz::default_http_status(error_kind::canceled) == 499);
  CHECK(qoderz::default_http_status(error_kind::internal) == 500);
}

TEST_CASE("make_error carries kind, status and code", "[common][error]") {
  auto const err = qoderz::make_error(error_kind::bad_request, "bad input", "invalid_json");
  CHECK(err.m_kind == error_kind::bad_request);
  CHECK(err.m_message == "bad input");
  CHECK(err.m_http_status == 400);
  CHECK(err.m_code == "invalid_json");
  CHECK_FALSE(err.m_retry_after.has_value());
}

TEST_CASE("quota_error carries retry_after", "[common][error]") {
  auto const err = qoderz::quota_error("slow down", "quota_exceeded", 17);
  CHECK(err.m_kind == error_kind::quota);
  CHECK(err.m_http_status == 429);
  CHECK(err.m_retry_after.has_value());
  CHECK(err.m_retry_after.value() == 17);
}

TEST_CASE("named helpers set the matching kind", "[common][error]") {
  CHECK(qoderz::configuration_error("a").m_kind == error_kind::configuration);
  CHECK(qoderz::auth_error("b").m_kind == error_kind::auth);
  CHECK(qoderz::bad_request_error("c").m_kind == error_kind::bad_request);
  CHECK(qoderz::upstream_error("d").m_kind == error_kind::upstream);
  CHECK(qoderz::internal_error("e").m_kind == error_kind::internal);
}

TEST_CASE("result holds either a value or an error", "[common][error]") {
  result<int> success = 42;
  REQUIRE(success.has_value());
  CHECK(*success == 42);

  result<int> failure = std::unexpected(qoderz::internal_error("boom", "unexpected_state"));
  REQUIRE_FALSE(failure.has_value());
  CHECK(failure.error().m_kind == error_kind::internal);
  CHECK(failure.error().m_message == "boom");
}
