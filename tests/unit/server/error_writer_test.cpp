#include "common/error.hpp"
#include "server/error_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

namespace {

using qoderz::error;
using qoderz::error_kind;
using qoderz::server::client_status_error;
using qoderz::server::http_status_for;
using qoderz::server::retry_after_for;
using qoderz::server::to_json;
using qoderz::server::to_sse_frame;
using qoderz::server::type_for;

error make(error_kind kind, std::string message = "boom") {
  return qoderz::make_error(kind, std::move(message));
}

} // namespace

TEST_CASE("Every error kind maps to its documented HTTP status and type", "[server][error]") {
  struct row {
    error_kind m_kind;
    int m_status;
    std::string_view m_type;
  };

  auto const rows = std::vector<row>{
      {error_kind::bad_request, 400, "invalid_request_error"},
      {error_kind::configuration, 500, "configuration_error"},
      {error_kind::auth, 401, "authentication_error"},
      {error_kind::quota, 429, "insufficient_quota"},
      {error_kind::busy, 503, "server_busy"},
      {error_kind::upstream, 502, "upstream_error"},
      {error_kind::io, 502, "upstream_error"},
      {error_kind::protocol, 502, "upstream_error"},
      {error_kind::canceled, 499, "cancelled"},
      {error_kind::internal, 500, "internal_error"},
  };

  for (auto const& entry : rows) {
    auto const value = make(entry.m_kind);
    CHECK(http_status_for(value) == entry.m_status);
    CHECK(type_for(value) == entry.m_type);
  }
}

TEST_CASE("The status follows the error kind, not an upstream status", "[server][error]") {
  // A Qoder 500 surfaces as an upstream error, so the facade must not echo 500 for a 502 category.
  auto value = make(error_kind::upstream);
  value.m_http_status = 500;
  CHECK(http_status_for(value) == 502);

  auto quota = make(error_kind::quota);
  quota.m_http_status = 401;
  CHECK(http_status_for(quota) == 429);

  // A client status the router pinned is kept: 404/405/413 are the same 4xx category.
  CHECK(http_status_for(client_status_error(404, "missing")) == 404);
  CHECK(http_status_for(client_status_error(405, "wrong method")) == 405);
  CHECK(http_status_for(client_status_error(413, "too large")) == 413);

  // A non-4xx override is ignored.
  auto bogus = make(error_kind::bad_request);
  bogus.m_http_status = 599;
  CHECK(http_status_for(bogus) == 400);
}

TEST_CASE("The error body carries message, type and a numeric code", "[server][error]") {
  auto const body = to_json(qoderz::auth_error("invalid API key"));
  CHECK(body ==
        R"({"error":{"code":401,"message":"invalid API key","type":"authentication_error"}})");

  auto const not_found = to_json(client_status_error(404, "unknown endpoint"));
  CHECK(not_found.find(R"("code":404)") != std::string::npos);
  CHECK(not_found.find(R"("type":"invalid_request_error")") != std::string::npos);
}

TEST_CASE("An empty message falls back to a per-category sentence", "[server][error]") {
  CHECK(to_json(make(error_kind::upstream, "")).find("The request failed.") != std::string::npos);
  CHECK(to_json(make(error_kind::canceled, "")).find("The request was cancelled.") !=
        std::string::npos);
  CHECK(to_json(make(error_kind::internal, "")).find("An unexpected error occurred.") !=
        std::string::npos);
  CHECK(to_json(make(error_kind::auth, "")).find("The request failed.") != std::string::npos);
}

TEST_CASE("Only a busy error with a positive hint carries Retry-After", "[server][error]") {
  auto busy = make(error_kind::busy);
  busy.m_retry_after = 7;
  CHECK(retry_after_for(busy) == std::optional<int>{7});

  auto no_hint = make(error_kind::busy);
  CHECK_FALSE(retry_after_for(no_hint).has_value());

  auto zero = make(error_kind::busy);
  zero.m_retry_after = 0;
  CHECK_FALSE(retry_after_for(zero).has_value());

  auto negative = make(error_kind::busy);
  negative.m_retry_after = -3;
  CHECK_FALSE(retry_after_for(negative).has_value());

  auto quota = qoderz::quota_error("out of quota", {}, 30);
  CHECK_FALSE(retry_after_for(quota).has_value());
}

TEST_CASE("An SSE error frame is one data line and a blank line", "[server][error]") {
  auto const frame = to_sse_frame(qoderz::upstream_error("upstream exploded"));
  CHECK(frame.starts_with("data: {"));
  CHECK(frame.ends_with("\n\n"));
  CHECK(frame.find("\"code\":502") != std::string::npos);
  CHECK(frame.find("\"type\":\"upstream_error\"") != std::string::npos);
}
