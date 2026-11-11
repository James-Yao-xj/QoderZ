#include "common/error.hpp"
#include "common/log.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/usage/usage.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
#include "unit/qoder/stderr_capture.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using qoderz::error_kind;
using qoderz::config::account_record;
using qoderz::config::resolved_site;
using qoderz::qoder::usage::parse_usage;
using qoderz::qoder::usage::select_token;
using qoderz::qoder::usage::usage_service;
using qoderz::qoder::usage::usage_snapshot;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

constexpr std::string_view access_token = "test-access-token-not-real";
constexpr std::string_view device_token = "test-device-token-not-real";
constexpr std::string_view test_uid = "uid-test-not-real";
constexpr std::string_view usage_path = "/sash/api/v2/me/usage";

/// The shape the research notes recorded, with four windows and an absolute expiry.
constexpr std::string_view usage_body = R"({
  "displayMode":"qoder",
  "userType":"pro_trial",
  "expiresAt":1767225600000,
  "qoderUsage":{
    "userQuota":{"total":1000,"used":250,"unit":"credits"},
    "addOnQuota":{"total":500,"used":100,"unit":"credits"},
    "orgResourcePackage":{"cap":2000,"used":500,"name":"Team pool","unit":"credits"},
    "dedicatedResourcePackages":[
      {"total":300,"remaining":300,"unit":"credits"},
      {"total":0,"used":10}
    ]
  }
})";

account_record make_account() {
  account_record account;
  account.m_site = "qoder";
  account.m_name = "ada";
  account.m_access_token = std::string{access_token};
  account.m_device_token = std::string{device_token};
  account.m_uid = std::string{test_uid};
  account.m_machine_id = "machine-test-not-real";
  account.m_email = "tester@example.invalid";
  return account;
}

resolved_site make_site(stub_server& server) {
  auto site = qoderz::config::find_site("qoder-cn");
  REQUIRE(site.has_value());
  auto resolved = qoderz::config::resolve_site(**site, "");
  REQUIRE(resolved.has_value());
  auto overridden = qoderz::config::apply_loopback_override(*resolved, server.origin());
  REQUIRE(overridden.has_value());
  return std::move(*overridden);
}

std::string_view path_of(std::string_view target) {
  auto const query_start = target.find('?');
  return query_start == std::string_view::npos ? target : target.substr(0, query_start);
}

stub_response json_response(int status, std::string body) {
  stub_response response;
  response.m_status = status;
  response.m_body = std::move(body);
  response.m_headers.push_back(
      qoderz::net::http_header{.m_name = "Content-Type", .m_value = "application/json"});
  return response;
}

/// A stub that answers the usage endpoint and keeps the request it saw.
struct usage_stub {
  std::vector<stub_request> m_requests;
  int m_status = 200;
  std::string m_body{usage_body};

  stub_server make(asio::io_context& io) {
    return stub_server{io, [this](stub_request const& request) -> asio::awaitable<stub_response> {
                         m_requests.push_back(request);
                         co_return json_response(m_status, m_body);
                       }};
  }
};

std::vector<std::string> names_of(usage_snapshot const& snapshot) {
  std::vector<std::string> names;
  names.reserve(snapshot.m_windows.size());
  for (auto const& window : snapshot.m_windows) {
    names.push_back(window.m_name);
  }
  return names;
}

} // namespace

TEST_CASE("parse_usage reads the plan, the expiry and every window", "[qoder][usage]") {
  auto const parsed = parse_usage(usage_body);
  REQUIRE(parsed.has_value());
  auto const& snapshot = *parsed;

  REQUIRE(snapshot.m_plan.has_value());
  CHECK(*snapshot.m_plan == "pro_trial");
  REQUIRE(snapshot.m_until_ms.has_value());
  CHECK(*snapshot.m_until_ms == 1'767'225'600'000);

  CHECK(names_of(snapshot) ==
        std::vector<std::string>{"Credits", "Add-on credits", "Team pool", "Dedicated credits"});
  REQUIRE(snapshot.m_windows.size() == 4);
  CHECK(snapshot.m_windows[0].m_percent == 25);
  CHECK(snapshot.m_windows[1].m_percent == 20);
  CHECK(snapshot.m_windows[2].m_percent == 25);
  CHECK(snapshot.m_windows[3].m_percent == 0);
  CHECK(snapshot.m_windows[0].m_display == "250 / 1000 credits");
  CHECK(snapshot.m_windows[1].m_display == "100 / 500 credits");
  CHECK(snapshot.m_windows[2].m_display == "500 / 2000 credits");
  CHECK(snapshot.m_windows[3].m_display == "0 / 300 credits");
}

TEST_CASE("parse_usage derives used from remaining", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{"userQuota":{"total":300,"remaining":120}}
  })");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->m_windows.size() == 1);
  CHECK(parsed->m_windows[0].m_used == 180);
  CHECK(parsed->m_windows[0].m_display == "180 / 300 credits");
}

TEST_CASE("parse_usage clamps a percent above one hundred", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{"userQuota":{"total":100,"used":120}}
  })");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->m_windows.size() == 1);
  CHECK(parsed->m_windows[0].m_percent == 100);
  CHECK(parsed->m_windows[0].m_display == "120 / 100 credits");
}

TEST_CASE("parse_usage reads snake_case fields", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "display_mode":"qoder",
    "user_type":"pro",
    "expires_at":1767225600,
    "qoder_usage":{"user_quota":{"total":100,"used":40,"unit":"times"}}
  })");
  REQUIRE(parsed.has_value());

  REQUIRE(parsed->m_plan.has_value());
  CHECK(*parsed->m_plan == "pro");
  REQUIRE(parsed->m_until_ms.has_value());
  CHECK(*parsed->m_until_ms == 1'767'225'600'000);

  REQUIRE(parsed->m_windows.size() == 1);
  CHECK(parsed->m_windows[0].m_unit == "times");
  CHECK(parsed->m_windows[0].m_percent == 40);
  CHECK(parsed->m_windows[0].m_display == "40 / 100 times");
}

TEST_CASE("parse_usage reads the plan and expiry from the usage block", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{"userType":"team","expires_at":"2026-01-01T00:00:00Z","userQuota":{"total":10,"used":1}}
  })");
  REQUIRE(parsed.has_value());

  REQUIRE(parsed->m_plan.has_value());
  CHECK(*parsed->m_plan == "team");
  REQUIRE(parsed->m_until_ms.has_value());
  CHECK(*parsed->m_until_ms == 1'767'225'600'000);
}

TEST_CASE("parse_usage reports the enterprise plan without windows", "[qoder][usage]") {
  for (auto const body : {R"({"displayMode":"enterprise","userType":"enterprise"})",
                          R"({"display_mode":"ENTERPRISE"})"}) {
    auto const parsed = parse_usage(body);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->m_plan.has_value());
    CHECK(*parsed->m_plan == "Enterprise");
    CHECK_FALSE(parsed->m_until_ms.has_value());
    CHECK(parsed->m_windows.empty());
  }
}

TEST_CASE("parse_usage rejects a reply without a quota block", "[qoder][usage]") {
  for (auto const body : {R"({"displayMode":"qoder","userType":"pro"})",
                          R"({"userType":"pro","qoderUsage":{"userQuota":{"total":1,"used":0}}})",
                          R"({"displayMode":"something","qoderUsage":{}})",
                          R"({"displayMode":"qoder","qoderUsage":"not-an-object"})", "not json"}) {
    auto const parsed = parse_usage(body);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().m_kind == error_kind::upstream);
    CHECK(parsed.error().m_message == "Qoder: missing quota data");
  }
}

TEST_CASE("parse_usage drops windows with an unusable total", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{
      "userQuota":{"total":0,"used":10},
      "addOnQuota":{"total":-5,"used":1},
      "orgResourcePackage":{"total":"2000","used":500},
      "dedicatedResourcePackages":[{"total":null,"used":1}]
    }
  })");
  REQUIRE(parsed.has_value());
  CHECK(parsed->m_windows.empty());
}

TEST_CASE("parse_usage drops a window whose amount is not a number", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{
      "userQuota":{"total":100,"used":"ten"},
      "addOnQuota":{"total":100,"remaining":"ten"},
      "orgResourcePackage":{"total":100},
      "dedicatedResourcePackages":[{"total":100,"used":-1}]
    }
  })");
  REQUIRE(parsed.has_value());
  CHECK(parsed->m_windows.empty());
}

TEST_CASE("parse_usage keeps five windows in the documented order", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{
      "userQuota":{"total":100,"used":1},
      "addOnQuota":{"total":200,"used":2},
      "orgResourcePackage":{"total":300,"used":3},
      "dedicatedResourcePackages":[
        {"total":400,"used":4,"name":"Team A"},
        {"total":500,"used":5,"name":"Team B"}
      ]
    }
  })");
  REQUIRE(parsed.has_value());
  CHECK(names_of(*parsed) == std::vector<std::string>{"Credits", "Add-on credits", "Shared credits",
                                                      "Team A", "Team B"});
  CHECK(parsed->m_windows.size() == 5);
}

TEST_CASE("parse_usage formats fractional amounts with two decimals", "[qoder][usage]") {
  auto const parsed = parse_usage(R"({
    "displayMode":"qoder",
    "qoderUsage":{
      "userQuota":{"total":3,"used":1.5},
      "addOnQuota":{"total":1000,"used":333.333},
      "orgResourcePackage":{"total":7,"used":2}
    }
  })");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->m_windows.size() == 3);
  CHECK(parsed->m_windows[0].m_display == "1.5 / 3 credits");
  CHECK(parsed->m_windows[1].m_display == "333.33 / 1000 credits");
  CHECK(parsed->m_windows[2].m_display == "2 / 7 credits");
}

TEST_CASE("parse_usage rejects an out-of-range expiry", "[qoder][usage]") {
  auto const far = parse_usage(R"({
    "displayMode":"qoder","expiresAt":253402300800000,
    "qoderUsage":{"userQuota":{"total":10,"used":1}}
  })");
  REQUIRE(far.has_value());
  CHECK_FALSE(far->m_until_ms.has_value());

  // A value below 1e11 is seconds, so it must still convert instead of looking like 1970.
  auto const seconds = parse_usage(R"({
    "displayMode":"qoder","expiresAt":1767225600,
    "qoderUsage":{"userQuota":{"total":10,"used":1}}
  })");
  REQUIRE(seconds.has_value());
  REQUIRE(seconds->m_until_ms.has_value());
  CHECK(*seconds->m_until_ms == 1'767'225'600'000);
}

TEST_CASE("select_token follows the pat and device rules", "[qoder][usage]") {
  auto account = make_account();
  CHECK(select_token(account) == device_token);

  account.m_device_token.clear();
  CHECK(select_token(account) == access_token);

  account.m_pat_auth = true;
  CHECK(select_token(account) == access_token);

  account.m_device_token = std::string{device_token};
  CHECK(select_token(account) == device_token);
}

TEST_CASE("usage_service sends the documented headers", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE(stub.m_requests.size() == 1);

  auto const& request = stub.m_requests.front();
  CHECK(request.m_method == "GET");
  CHECK(path_of(request.m_target) == usage_path);
  CHECK(request.m_body.empty());

  auto const* authorization = qoderz::net::header_value(request.m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(*authorization == "Bearer test-device-token-not-real");

  for (auto const& [name, value] :
       std::vector<std::pair<std::string, std::string>>{{"Cosy-ClientType", "10"},
                                                        {"User-Agent", "Qoder"},
                                                        {"Accept", "application/json"},
                                                        {"Accept-Encoding", "identity"}}) {
    auto const* header = qoderz::net::header_value(request.m_headers, name);
    REQUIRE(header != nullptr);
    CHECK(*header == value);
  }
}

TEST_CASE("usage_service uses the access token for a PAT account without a device token",
          "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto account = make_account();
  account.m_pat_auth = true;
  account.m_device_token.clear();

  auto outcome = run_awaitable(io, service.fetch(site, account));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE(stub.m_requests.size() == 1);

  auto const* authorization =
      qoderz::net::header_value(stub.m_requests.front().m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(*authorization == "Bearer test-access-token-not-real");
}

TEST_CASE("usage_service calls the openapi base", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  // The two other bases point at a closed port; reaching them would fail the fetch.
  auto site = make_site(server);
  site.m_api_base = "http://127.0.0.1:9";
  site.m_web_base = "http://127.0.0.1:9";

  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("usage_service maps a 401 to an auth error", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  stub.m_status = 401;
  stub.m_body = R"({"code":105,"message":"token expired"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_http_status == 401);
}

TEST_CASE("usage_service maps a 429 to a quota error", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  stub.m_status = 429;
  stub.m_body = R"({"message":"quota exceeded"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::quota);
}

TEST_CASE("usage_service maps a 502 to an upstream error", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  stub.m_status = 502;
  stub.m_body = "bad gateway";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
}

TEST_CASE("usage_service reports a 200 without quota data as upstream", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  stub.m_body = R"({"displayMode":"qoder","userType":"pro"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message == "Qoder: missing quota data");
}

TEST_CASE("usage_service never logs the tokens", "[qoder][usage]") {
  asio::io_context io;
  usage_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  usage_service service{http, clock};

  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(qoderz::log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  auto outcome = run_awaitable(io, service.fetch(site, make_account()));
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.find(access_token) == std::string::npos);
  CHECK(logged.find(device_token) == std::string::npos);
  CHECK(logged.find("tester@example.invalid") == std::string::npos);
  CHECK(logged.find("Bearer") == std::string::npos);
}
