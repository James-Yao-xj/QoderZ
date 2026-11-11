#include "common/error.hpp"
#include "common/log.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/catalog/listing.hpp"
#include "qoder/catalog/service.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
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
using qoderz::config::account_record;
using qoderz::config::resolved_site;
using qoderz::qoder::catalog::catalog_service;
using qoderz::qoder::catalog::fallback_models;
using qoderz::qoder::catalog::model_info;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

constexpr std::string_view access_token = "test-access-token-not-real";
constexpr std::string_view test_uid = "uid-test-not-real";
constexpr std::string_view listing_path = "/algo/api/v2/model/list";

/// Two enabled models, one of which is disabled and one filtered, so a merged result is visible in
/// the count as well as in the order.
constexpr std::string_view listing_body = R"({
  "app":[{"key":"alpha","display_name":"Alpha","enable":true,"max_input_tokens":1000}],
  "chat":[{"key":"beta","display_name":"Beta","enable":true,"max_input_tokens":2000}]
})";

account_record make_account() {
  account_record account;
  account.m_site = "qoder";
  account.m_name = "ada";
  account.m_access_token = std::string{access_token};
  account.m_uid = std::string{test_uid};
  account.m_machine_id = "machine-test-not-real";
  account.m_display_name = "Ada Test";
  account.m_email = "tester@example.invalid";
  return account;
}

/// The loopback site, so no name is ever resolved and no real host is contacted.
resolved_site make_site(stub_server& server) {
  auto site = qoderz::config::find_site("qoder");
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

/// A stub that answers every model-list request with `body` and remembers what it saw.
struct listing_stub {
  std::vector<stub_request> m_requests;
  int m_status = 200;
  std::string m_body{listing_body};

  stub_server make(asio::io_context& io) {
    return stub_server{io, [this](stub_request const& request) -> asio::awaitable<stub_response> {
                         m_requests.push_back(request);
                         co_return json_response(m_status, m_body);
                       }};
  }

  [[nodiscard]] std::size_t listing_calls() const {
    std::size_t total = 0;
    for (auto const& request : m_requests) {
      if (path_of(request.m_target) == listing_path) {
        ++total;
      }
    }
    return total;
  }
};

std::vector<std::string> keys_of(std::vector<model_info> const& models) {
  std::vector<std::string> keys;
  keys.reserve(models.size());
  for (auto const& model : models) {
    keys.push_back(model.m_key);
  }
  return keys;
}

/// The fallback entry for `key`, or nullptr when the built-in list does not carry it.
model_info const* model_of(std::vector<model_info> const& models, std::string_view key) {
  for (auto const& model : models) {
    if (model.m_key == key) {
      return &model;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("catalog_service fetches the listing and parses the merged models",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(keys_of(**outcome) == std::vector<std::string>{"alpha", "beta"});
  CHECK(stub.listing_calls() == 1);
}

TEST_CASE("catalog_service sends a signed GET with the listing query",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE(stub.m_requests.size() == 1);

  auto const& request = stub.m_requests.front();
  CHECK(request.m_method == "GET");
  CHECK(request.m_target == "/algo/api/v2/model/list?Encode=1");
  CHECK(request.m_body.empty());

  auto const* authorization = qoderz::net::header_value(request.m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(authorization->starts_with("Bearer COSY."));

  auto const* user = qoderz::net::header_value(request.m_headers, "Cosy-User");
  REQUIRE(user != nullptr);
  CHECK(*user == test_uid);

  auto const* accept = qoderz::net::header_value(request.m_headers, "Accept");
  REQUIRE(accept != nullptr);
  CHECK(*accept == "application/json");

  auto const* key = qoderz::net::header_value(request.m_headers, "Cosy-Key");
  REQUIRE(key != nullptr);
  CHECK_FALSE(key->empty());
}

TEST_CASE("catalog_service answers the second call from the cache", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto first = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  auto second = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());

  CHECK(stub.listing_calls() == 1);
  CHECK(keys_of(**second) == keys_of(**first));
}

TEST_CASE("catalog_service refetches after the cache expires", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  clock.advance(qoderz::qoder::catalog::cache_ttl - 1ms);
  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  CHECK(stub.listing_calls() == 1);

  clock.advance(1ms);
  auto after = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(after.has_value());
  REQUIRE(after->has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service refetches when force_refresh asks for it", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  auto forced = run_awaitable(io, service.fetch(site, account, true));
  REQUIRE(forced.has_value());
  REQUIRE(forced->has_value());
  CHECK(stub.listing_calls() == 2);

  // The forced result was stored, so the next normal call is a cache hit again.
  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service caches per account and site", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto const first = make_account();
  auto second = make_account();
  second.m_uid = "uid-other-test-not-real";

  REQUIRE(run_awaitable(io, service.fetch(site, first, false)).has_value());
  REQUIRE(run_awaitable(io, service.fetch(site, first, false)).has_value());
  CHECK(stub.listing_calls() == 1);

  REQUIRE(run_awaitable(io, service.fetch(site, second, false)).has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service returns the built-in list for an empty listing",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_body = R"({"app":[],"chat":[]})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto first = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  CHECK(keys_of(**first) == keys_of(fallback_models(site.m_site_id)));

  // Nothing was cached, so the next call goes back to the network.
  auto second = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service returns the built-in list for an unusable body",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_body = "not json at all";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(keys_of(**outcome) == keys_of(fallback_models(site.m_site_id)));
}

TEST_CASE("catalog_service maps a 401 to an auth error", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_status = 401;
  stub.m_body = R"({"code":105,"message":"token expired"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_http_status == 401);
}

TEST_CASE("catalog_service maps a 429 to a quota error", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_status = 429;
  stub.m_body = R"({"code":110,"message":"quota exhausted"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::quota);
}

TEST_CASE("catalog_service maps a 500 to an upstream error", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_status = 500;
  stub.m_body = "boom";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(stub.listing_calls() == 1);
}

TEST_CASE("a failed fetch is not cached", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  stub.m_status = 503;
  stub.m_body = R"({"code":10605,"message":"queue is full"})";
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto failed = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(failed.has_value());
  REQUIRE_FALSE(failed->has_value());
  CHECK(failed->error().m_kind == error_kind::busy);

  stub.m_status = 200;
  stub.m_body = std::string{listing_body};
  auto recovered = run_awaitable(io, service.fetch(site, account, false));
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service keeps at most the configured number of entries",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto const limit = qoderz::qoder::catalog::max_cache_entries;
  for (std::size_t index = 0; index < limit + 2; ++index) {
    auto account = make_account();
    account.m_uid = "uid-" + std::to_string(index) + "-test-not-real";
    auto outcome = run_awaitable(io, service.fetch(site, account, false));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    clock.advance(1ms);
  }
  CHECK(stub.listing_calls() == limit + 2);

  // The earliest uid was evicted, so asking for it again reaches the network a second time.
  auto stale = make_account();
  stale.m_uid = "uid-0-test-not-real";
  REQUIRE(run_awaitable(io, service.fetch(site, stale, false)).has_value());
  CHECK(stub.listing_calls() == limit + 3);

  // The newest uid is still cached.
  auto fresh = make_account();
  fresh.m_uid = "uid-" + std::to_string(limit + 1) + "-test-not-real";
  REQUIRE(run_awaitable(io, service.fetch(site, fresh, false)).has_value());
  CHECK(stub.listing_calls() == limit + 3);
}

TEST_CASE("catalog_service calls the api base, not the web or openapi base",
          "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  // Both other bases point at a port nothing listens on: using either would fail the fetch.
  auto site = make_site(server);
  site.m_web_base = "http://127.0.0.1:9";
  site.m_openapi_base = "http://127.0.0.1:9";

  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(stub.listing_calls() == 1);
}

TEST_CASE("catalog_service keys the cache by vpc", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto site = make_site(server);
  auto const account = make_account();
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  CHECK(stub.listing_calls() == 1);

  // The same account reached through a VPC instance is a different catalogue: no shared entry.
  site.m_vpc = "acme";
  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  CHECK(stub.listing_calls() == 2);

  REQUIRE(run_awaitable(io, service.fetch(site, account, false)).has_value());
  CHECK(stub.listing_calls() == 2);
}

TEST_CASE("catalog_service never logs the access token", "[qoder][catalog][service]") {
  asio::io_context io;
  listing_stub stub;
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  auto const site = make_site(server);
  qoderz::net::http_client http{io};
  fake_clock clock;
  catalog_service service{http, clock};

  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(qoderz::log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  auto outcome = run_awaitable(io, service.fetch(site, make_account(), false));
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.find(access_token) == std::string::npos);
  CHECK(logged.find("tester@example.invalid") == std::string::npos);
  CHECK(logged.find("machine-test-not-real") == std::string::npos);
  CHECK(logged.find("COSY.") == std::string::npos);
}

TEST_CASE("fallback_models carries the documented built-in lists", "[qoder][catalog][service]") {
  auto const& international = fallback_models("qoder");
  auto const& china = fallback_models("qoder-cn");

  CHECK(international.size() == 17);
  // The CN site serves a subset of the international list. Listing more would only hand the user a
  // model the site answers with `unknown or disabled model`.
  CHECK(china.size() == 4);
  CHECK(international.front().m_key == "ultimate");
  CHECK(china.front().m_key == "ultimate");
  CHECK(keys_of(china) == std::vector<std::string>{"ultimate", "performance", "efficient", "lite"});
  CHECK(china.back().m_key == "lite");
  CHECK_FALSE(china.back().m_images);
  CHECK(china.back().m_raw_json == "{}");

  for (auto const& model : international) {
    CHECK(model.m_images);
    CHECK(model.m_source.empty());
    CHECK_FALSE(model.m_always_thinks);
    CHECK_FALSE(model.m_free);
    CHECK(model.m_rate == 0);
    CHECK(model.m_rate_was == 0);
    CHECK(model.m_default_window == model.m_context);
  }

  // Spot-check the ported data: a wrong context or effort ladder is invisible in a count check.
  auto const* ultimate = model_of(international, "ultimate");
  REQUIRE(ultimate != nullptr);
  CHECK(ultimate->m_context == 1'000'000);
  CHECK(ultimate->m_efforts == std::vector<std::string>{"low", "medium", "high", "xhigh", "max"});

  auto const* qwen = model_of(international, "qmodel_38max");
  REQUIRE(qwen != nullptr);
  CHECK(qwen->m_name == "Qwen3.8-Max");
  CHECK(qwen->m_context == 180'000);
  CHECK(qwen->m_efforts == std::vector<std::string>{"low", "medium", "xhigh"});

  auto const* kimi = model_of(international, "kmodel");
  REQUIRE(kimi != nullptr);
  CHECK(kimi->m_context == 0);
  CHECK(kimi->m_efforts == std::vector<std::string>{"low", "high", "max"});

  auto const* lite = model_of(china, "lite");
  REQUIRE(lite != nullptr);
  CHECK(lite->m_context == 0);
  CHECK(lite->m_efforts.empty());

  // An unknown site still gets a usable list rather than "no models".
  CHECK(fallback_models("unknown").size() == 17);
}
