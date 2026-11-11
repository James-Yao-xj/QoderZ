#include "common/error.hpp"
#include "common/log.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "config/site.hpp"
#include "qoder/auth/pat.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
#include "unit/qoder/stderr_capture.hpp"

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
using qoderz::config::resolved_site;
using qoderz::config::site_definition;
using qoderz::qoder::auth::login_with_pat;
using qoderz::qoder::auth::validate_pat;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

constexpr std::string_view test_pat = "pt-test-not-real";
constexpr std::string_view access_token = "test-access-token-not-real";
constexpr std::string_view test_uid = "uid-test-not-real";

std::string_view path_of(std::string_view target) {
  auto const query_start = target.find('?');
  return query_start == std::string_view::npos ? target : target.substr(0, query_start);
}

stub_response json_response(int status, std::string body) {
  stub_response response;
  response.m_status = status;
  response.m_body = std::move(body);
  return response;
}

struct fixture {
  fake_clock m_clock;
  site_definition const* m_site = nullptr;
  resolved_site m_resolved;
  bool m_ready = false;

  explicit fixture(stub_server& server) {
    auto site = qoderz::config::find_site("qoder");
    if (!site) {
      return;
    }
    m_site = *site;
    auto resolved = qoderz::config::resolve_site(*m_site, "");
    if (!resolved) {
      return;
    }
    auto overridden = qoderz::config::apply_loopback_override(*resolved, server.origin());
    if (!overridden) {
      return;
    }
    m_resolved = std::move(*overridden);
    m_ready = true;
  }
};

} // namespace

TEST_CASE("a PAT is validated before anything is sent", "[qoder][auth][pat]") {
  CHECK_FALSE(validate_pat("").has_value());
  CHECK_FALSE(validate_pat("   ").has_value());
  CHECK_FALSE(validate_pat("test-token-not-real").has_value());
  CHECK_FALSE(validate_pat("PT-upper").has_value());
  CHECK(validate_pat("pt-test-not-real").has_value());
  CHECK(validate_pat("  pt-test-not-real  ").value() == test_pat);

  CHECK(validate_pat("").error().m_kind == error_kind::configuration);
  CHECK(validate_pat("nope").error().m_message.find("pt-") != std::string::npos);

  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200, "");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto rejected = run_awaitable(
      io, login_with_pat(http, *env.m_site, env.m_resolved, "not-a-pat", env.m_clock));
  REQUIRE(rejected.has_value());
  REQUIRE_FALSE(rejected->has_value());
  CHECK(rejected->error().m_kind == error_kind::configuration);
  CHECK(server.request_count() == 0);
}

TEST_CASE("a PAT is exchanged for an access token and an account", "[qoder][auth][pat]") {
  asio::io_context io;
  std::vector<stub_request> seen;
  stub_server server{io, [&seen](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.push_back(request);
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/jobToken/exchange") {
                         co_return json_response(200, R"({"token":"test-access-token-not-real",)"
                                                      R"("refresh_token":"test-refresh-not-real",)"
                                                      R"("expires_in":900000})");
                       }
                       co_return json_response(200,
                                               R"({"email":"tester@example.invalid",)"
                                               R"("name":"Ada Test","id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto outcome = run_awaitable(
      io, login_with_pat(http, *env.m_site, env.m_resolved, "  pt-test-not-real  ", env.m_clock));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& record = outcome->value();

  CHECK(record.m_site == "qoder");
  CHECK(record.m_pat_auth);
  CHECK(record.m_pat == test_pat);
  CHECK(record.m_access_token == access_token);
  CHECK(record.m_uid == test_uid);
  CHECK(record.m_email == "tester@example.invalid");
  CHECK(record.m_display_name == "Ada Test");
  CHECK(record.m_account_id == "tester@example.invalid");
  CHECK(record.m_device_chat == false);
  CHECK(record.m_device_token.empty());
  CHECK(record.m_device_refresh_token.empty());
  CHECK(record.m_machine_id.size() == 36);
  CHECK(record.m_created_at == env.m_clock.now_ms());
  CHECK(record.m_expires_at == env.m_clock.now_ms() + 900000);

  REQUIRE(seen.size() == 2);
  CHECK(seen[0].m_method == "POST");
  CHECK(path_of(seen[0].m_target) == "/api/v1/jobToken/exchange");
  CHECK(seen[0].m_body == std::string{"{\"personal_token\":\""} + std::string{test_pat} + "\"}");
  CHECK(qoderz::net::header_value(seen[0].m_headers, "Authorization") == nullptr);

  CHECK(seen[1].m_method == "GET");
  CHECK(path_of(seen[1].m_target) == "/api/v1/userinfo");
  auto const* authorization = qoderz::net::header_value(seen[1].m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(*authorization == std::string{"Bearer "} + std::string{access_token});
}

TEST_CASE("a PAT without an expiry gets one day", "[qoder][auth][pat]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       if (path_of(request.m_target) == "/api/v1/jobToken/exchange") {
                         co_return json_response(200, R"({"token":"test-access-token-not-real"})");
                       }
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto outcome =
      run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_expires_at ==
        env.m_clock.now_ms() + std::chrono::hours{24} / std::chrono::milliseconds{1});
}

TEST_CASE("a refused PAT is an auth error", "[qoder][auth][pat]") {
  for (int const status : {400, 401, 403}) {
    asio::io_context io;
    stub_server server{io, [status](stub_request const&) -> asio::awaitable<stub_response> {
                         co_return json_response(status, R"({"message":"nope"})");
                       }};
    REQUIRE(server.start().has_value());

    qoderz::net::http_client http{io};
    fixture env{server};
    REQUIRE(env.m_ready);

    auto outcome =
        run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    INFO("status " << status);
    CHECK(outcome->error().m_kind == error_kind::auth);
  }
}

TEST_CASE("an exchange failure is an upstream error", "[qoder][auth][pat]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(500, R"({"message":"boom"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto outcome =
      run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_http_status == 500);
}

TEST_CASE("an exchange without a token is an upstream error", "[qoder][auth][pat]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200, R"({"refresh_token":"only-a-refresh"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto outcome =
      run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("no token") != std::string::npos);
}

TEST_CASE("a failed userinfo call is an auth error", "[qoder][auth][pat]") {
  for (int const status : {401, 403, 500}) {
    asio::io_context io;
    stub_server server{io, [status](stub_request const& request) -> asio::awaitable<stub_response> {
                         if (path_of(request.m_target) == "/api/v1/jobToken/exchange") {
                           co_return json_response(200,
                                                   R"({"token":"test-access-token-not-real"})");
                         }
                         co_return json_response(status, R"({"message":"nope"})");
                       }};
    REQUIRE(server.start().has_value());

    qoderz::net::http_client http{io};
    fixture env{server};
    REQUIRE(env.m_ready);

    auto outcome =
        run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    INFO("status " << status);
    CHECK(outcome->error().m_kind == error_kind::auth);
  }
}

TEST_CASE("a userinfo call without a uid is an auth error", "[qoder][auth][pat]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       if (path_of(request.m_target) == "/api/v1/jobToken/exchange") {
                         co_return json_response(200, R"({"token":"test-access-token-not-real"})");
                       }
                       co_return json_response(200, R"({"email":"tester@example.invalid"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto outcome =
      run_awaitable(io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-x", env.m_clock));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_message == "Qoder returned no account id for the PAT");
}

TEST_CASE("a PAT never reaches the log", "[qoder][auth][pat]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       if (path_of(request.m_target) == "/api/v1/jobToken/exchange") {
                         co_return json_response(200, R"({"token":"test-access-token-not-real"})");
                       }
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);

  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(qoderz::log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  auto outcome = run_awaitable(
      io, login_with_pat(http, *env.m_site, env.m_resolved, "pt-test-not-real", env.m_clock));
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.find("pt-test-not-real") == std::string::npos);
  CHECK(logged.find("test-access-token-not-real") == std::string::npos);
}
