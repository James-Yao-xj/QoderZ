#include "common/error.hpp"
#include "common/log.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "qoder/auth/device.hpp"
#include "qoder/auth/types.hpp"
#include "support/async_test.hpp"
#include "support/stub_server.hpp"
#include "unit/qoder/auth/device_fixture.hpp"
#include "unit/qoder/stderr_capture.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using qoderz::error_kind;
using qoderz::qoder::auth::device_flow;
using qoderz::qoder::auth::login_options;
using qoderz::qoder::auth::progress_event;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using namespace qoderz::test::device;

} // namespace

TEST_CASE("the device challenge is built without touching the network", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(503, "");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto const challenge = flow.make_challenge();
  CHECK(challenge.m_verifier.size() == 86);
  CHECK(challenge.m_challenge.size() == 43);
  CHECK(challenge.m_nonce.size() == 36);
  CHECK(challenge.m_machine_id.size() == 36);
  CHECK(challenge.m_nonce != challenge.m_machine_id);
  CHECK(challenge.m_verifier.find('=') == std::string::npos);
  CHECK(challenge.m_challenge.find('=') == std::string::npos);
  CHECK(server.request_count() == 0);

  auto const second = flow.make_challenge();
  CHECK(second.m_verifier != challenge.m_verifier);
  CHECK(second.m_nonce != challenge.m_nonce);
}

TEST_CASE("the device URL carries the challenge, nonce, machine id and client id",
          "[qoder][auth][device]") {
  asio::io_context io;
  recorder seen;
  stub_server server{io, [&](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.take(request);
                       if (path_of(request.m_target) == "/api/v1/deviceToken/poll") {
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       if (path_of(request.m_target) == "/api/v1/me/jobToken") {
                         co_return json_response(200,
                                                 R"({"token":"test-job-token-not-real",)"
                                                 R"("refresh_token":"test-job-refresh-not-real",)"
                                                 R"("expires_in":3600000})");
                       }
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  std::string opened_url;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock,
                   [&opened_url](std::string_view url) -> qoderz::result<void> {
                     opened_url = std::string{url};
                     return {};
                   }};

  std::vector<progress_event> events;
  auto outcome = run_awaitable(io, flow.login(login_options{}, collecting(events)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());

  // The device page is opened in a browser, never fetched, so only the reported URL is checked.
  CHECK(opened_url.rfind(server.origin() + "/device/selectAccounts?", 0) == 0);
  CHECK(opened_url == events.front().m_url);
  CHECK(events.front().m_instructions ==
        "Sign in to Qoder in the browser and authorize this device");
  CHECK(events.front().m_url.find("redirect_uri") == std::string::npos);
  CHECK(query_value(events.front().m_url, "challenge_method") ==
        std::optional<std::string>{"S256"});
  CHECK(query_value(events.front().m_url, "client_id") ==
        std::optional<std::string>{std::string{env.m_site->m_client_id}});
  REQUIRE(query_value(events.front().m_url, "challenge").has_value());
  REQUIRE(query_value(events.front().m_url, "nonce").has_value());
  REQUIRE(query_value(events.front().m_url, "machine_id").has_value());
  CHECK(query_value(events.front().m_url, "challenge")->size() == 43);

  auto const poll = seen.by_path("/api/v1/deviceToken/poll");
  REQUIRE(poll.has_value());
  CHECK(bearer_of(*poll).empty());
  CHECK(query_value(poll->m_target, "challenge_method") == std::optional<std::string>{"S256"});
  CHECK(query_value(poll->m_target, "nonce") == query_value(events.front().m_url, "nonce"));
  REQUIRE(query_value(poll->m_target, "verifier").has_value());
  CHECK(query_value(poll->m_target, "verifier")->size() == 86);
}

TEST_CASE("polling continues until a token arrives", "[qoder][auth][device]") {
  asio::io_context io;
  std::size_t polls = 0;
  stub_server server{io, [&polls](stub_request const& request) -> asio::awaitable<stub_response> {
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/deviceToken/poll") {
                         ++polls;
                         if (polls < 3) {
                           co_return json_response(200, R"({"token":""})");
                         }
                         co_return json_response(200, R"({"token":"test-device-token-not-real"})");
                       }
                       if (path == "/api/v1/me/jobToken") {
                         co_return json_response(200,
                                                 R"({"token":"test-job-token-not-real",)"
                                                 R"("refresh_token":"test-job-refresh-not-real"})");
                       }
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  login_options options;
  options.m_poll_interval = 1ms;
  options.m_timeout = 5s;
  std::vector<progress_event> events;
  auto outcome = run_awaitable(io, flow.login(options, collecting(events)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(polls == 3);
  CHECK(events.size() == 3);
}

TEST_CASE("the sign-in times out when no token ever arrives", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(404, "");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  login_options options;
  options.m_poll_interval = 1ms;
  options.m_timeout = 20ms;
  auto outcome = run_awaitable(io, flow.login(options));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_message == "the sign-in timed out; start again");
  CHECK(server.request_count() > 1);
}

TEST_CASE("a failed browser launch does not stop the sign-in", "[qoder][auth][device]") {
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
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  device_flow flow{
      http, *env.m_site, env.m_resolved, env.m_clock, [](std::string_view) -> qoderz::result<void> {
        return std::unexpected(qoderz::make_error(error_kind::io, "no display available"));
      }};

  auto outcome = run_awaitable(io, flow.login(login_options{}));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
}

TEST_CASE("a device sign-in never writes a token or an email to the log", "[qoder][auth][device]") {
  asio::io_context io;
  stub_server server{
      io, [](stub_request const& request) -> asio::awaitable<stub_response> {
        auto const path = path_of(request.m_target);
        if (path == "/api/v1/deviceToken/poll") {
          co_return json_response(200, R"({"token":"test-device-token-not-real",)"
                                       R"("refresh_token":"test-device-refresh-not-real"})");
        }
        if (path == "/api/v1/me/jobToken") {
          co_return json_response(200, R"({"token":"test-job-token-not-real",)"
                                       R"("refresh_token":"test-job-refresh-not-real"})");
        }
        co_return json_response(200, R"({"email":"tester@example.invalid",)"
                                     R"("id":"uid-test-not-real"})");
      }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  REQUIRE(env.m_ready);
  quiet_launcher launcher;
  device_flow flow{http, *env.m_site, env.m_resolved, env.m_clock, launcher};

  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(qoderz::log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  auto outcome = run_awaitable(io, flow.login(login_options{}));
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.find("test-device-token-not-real") == std::string::npos);
  CHECK(logged.find("test-device-refresh-not-real") == std::string::npos);
  CHECK(logged.find("test-job-token-not-real") == std::string::npos);
  CHECK(logged.find("test-job-refresh-not-real") == std::string::npos);
  CHECK(logged.find("tester@example.invalid") == std::string::npos);
}
