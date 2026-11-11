#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "qoder/auth/device.hpp"
#include "qoder/auth/types.hpp"
#include "support/async_test.hpp"
#include "support/stub_server.hpp"
#include "unit/qoder/auth/device_fixture.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using qoderz::error_kind;
using qoderz::qoder::auth::device_flow;
using qoderz::qoder::auth::login_options;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using namespace qoderz::test::device;

} // namespace

TEST_CASE("a device sign-in exchanges the poll token for a job token", "[qoder][auth][device]") {
  asio::io_context io;
  recorder seen;
  stub_server server{
      io, [&](stub_request const& request) -> asio::awaitable<stub_response> {
        seen.take(request);
        auto const path = path_of(request.m_target);
        if (path == "/api/v1/deviceToken/poll") {
          co_return json_response(200, R"({"token":"test-device-token-not-real",)"
                                       R"("refresh_token":"test-device-refresh-not-real"})");
        }
        if (path == "/api/v1/me/jobToken") {
          co_return json_response(200, R"({"token":"test-job-token-not-real",)"
                                       R"("refresh_token":"test-job-refresh-not-real",)"
                                       R"("expires_in":3600000})");
        }
        co_return json_response(200, R"({"email":"tester@example.invalid",)"
                                     R"("name":"Ada Test","id":"uid-test-not-real"})");
      }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  login_options options;
  options.m_requested_name = "work";
  auto outcome = run_awaitable(io, flow.login(options));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& record = outcome->value();

  CHECK(record.m_site == "qoder");
  CHECK(record.m_vpc.empty());
  CHECK(record.m_name == "work");
  CHECK(record.m_access_token == job_token);
  CHECK(record.m_refresh_token == job_refresh);
  CHECK(record.m_device_token == poll_token);
  CHECK(record.m_device_refresh_token == poll_refresh);
  CHECK(record.m_device_chat == false);
  CHECK(record.m_pat_auth == false);
  CHECK(record.m_pat.empty());
  CHECK(record.m_uid == test_uid);
  CHECK(record.m_email == test_email);
  CHECK(record.m_display_name == "Ada Test");
  CHECK(record.m_account_id == test_email);
  CHECK(record.m_machine_id.size() == 36);
  CHECK(record.m_created_at == env.m_clock.now_ms());
  CHECK(record.m_expires_at == env.m_clock.now_ms() + 3600000);

  auto const exchange = seen.by_path("/api/v1/me/jobToken");
  REQUIRE(exchange.has_value());
  CHECK(exchange->m_method == "POST");
  CHECK(exchange->m_body ==
        std::string{"{\"clientId\":\""} + std::string{env.m_site->m_client_id} + "\"}");
  CHECK(bearer_of(*exchange) == poll_token);
  auto const* content_type = qoderz::net::header_value(exchange->m_headers, "Content-Type");
  REQUIRE(content_type != nullptr);
  CHECK(*content_type == "application/json");

  auto const userinfo = seen.by_path("/api/v1/userinfo");
  REQUIRE(userinfo.has_value());
  CHECK(bearer_of(*userinfo) == poll_token);
}

TEST_CASE("a CN device sign-in falls back to the device token pair", "[qoder][auth][device]") {
  asio::io_context io;
  recorder seen;
  stub_server server{io, [&](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.take(request);
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(
                             200, R"({"token":"test-device-token-not-real",)"
                                  R"("refresh_token":"test-device-refresh-not-real",)"
                                  R"("user_id":"uid-from-poll-not-real","user_name":"Poll Name",)"
                                  R"("expires_in":7200})");
                       }
                       if (path == "/api/v1/me/jobToken") {
                         co_return json_response(400, R"({"code":"105","message":"not eligible"})");
                       }
                       co_return json_response(500, "userinfo is not available");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server, "qoder-cn"};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& record = outcome->value();

  CHECK(record.m_site == "qoder-cn");
  CHECK(record.m_device_chat);
  CHECK(record.m_access_token == poll_token);
  CHECK(record.m_refresh_token == poll_refresh);
  CHECK(record.m_device_token == poll_token);
  CHECK(record.m_device_refresh_token == poll_refresh);
  CHECK(record.m_expires_at == env.m_clock.now_ms() + 7200 * 1000);
  CHECK(record.m_uid == "uid-from-poll-not-real");
  CHECK(record.m_display_name == "Poll Name");
  CHECK(record.m_email.empty());
  CHECK(record.m_account_id == "uid-from-poll-not-real");
}

TEST_CASE("a 4xx from the exchange classifies by site and status, not by the message",
          "[qoder][auth][device]") {
  struct expectation {
    std::string_view m_site_id;
    int m_status;
    error_kind m_kind;
    std::string_view m_message_part;
  };

  // The CN 400 case is the fallback path and is covered above. These three all fail, and each one
  // failed for the wrong reason while the branch was chosen by the message `explain()` produced: a
  // 409 fell through to `upstream`, and a 429 was rewritten into `quota`.
  std::vector<expectation> const cases{
      {"qoder", 409, error_kind::auth, "409"},
      {"qoder", 429, error_kind::auth, "429"},
      {"qoder-cn", 429, error_kind::quota, ""},
  };

  for (auto const& expected : cases) {
    INFO("site " << expected.m_site_id << " status " << expected.m_status);
    asio::io_context io;
    stub_server server{
        io, [&expected](stub_request const& request) -> asio::awaitable<stub_response> {
          auto const path = path_of(request.m_target);
          if (path == "/api/v1/deviceToken/poll") {
            co_return json_response(200, R"({"token":"test-device-token-not-real",)"
                                         R"("refresh_token":"test-device-refresh-not-real",)"
                                         R"("expires_in":7200})");
          }
          co_return json_response(expected.m_status, R"({"message":"nope"})");
        }};
    REQUIRE(server.start().has_value());

    qoderz::net::http_client http{io};
    fixture env{server, expected.m_site_id};
    REQUIRE(env.m_ready);
    quiet_launcher launcher;
    device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

    auto outcome = run_awaitable(io, flow.login(login_options{}));
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().m_kind == expected.m_kind);
    if (!expected.m_message_part.empty()) {
      CHECK(outcome->error().m_message.find(expected.m_message_part) != std::string::npos);
    }
  }
}

TEST_CASE("a refused exchange on the international site is an auth error",
          "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       co_return json_response(401, R"({"code":"105","message":"no"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
}

TEST_CASE("an exchange without a job token is an auth error", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       co_return json_response(200, R"({"refresh_token":"only-refresh"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_message.find("job token") != std::string::npos);
}

TEST_CASE("an unexpected exchange failure is an upstream error", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       co_return json_response(500, R"({"message":"boom"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("HTTP 500") != std::string::npos);
  CHECK(outcome->error().m_message.find("boom") != std::string::npos);
}

TEST_CASE("a VPC sign-in without a uid is an auth error", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       if (path == "/api/v1/me/jobToken") {
                         co_return json_response(200,
                                                 R"({"token":"test-job-token-not-real",)"
                                                 R"("refresh_token":"test-job-refresh-not-real"})");
                       }
                       co_return json_response(200, R"({"email":"tester@example.invalid"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server, "qoder-cn"};
  REQUIRE(env.m_ready);
  env.m_resolved.m_vpc = "acme";
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_message == "Qoder returned no account id for the VPC sign-in");
}

TEST_CASE("a VPC sign-in stamps the account id with the instance", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       if (path == "/api/v1/me/jobToken") {
                         co_return json_response(200,
                                                 R"({"token":"test-job-token-not-real",)"
                                                 R"("refresh_token":"test-job-refresh-not-real"})");
                       }
                       co_return json_response(200, R"({"name":"Ada","id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server, "qoder-cn"};
  REQUIRE(env.m_ready);
  env.m_resolved.m_vpc = "acme";
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_vpc == "acme");
  CHECK(outcome->value().m_account_id == "uid-test-not-real (acme)");
}
