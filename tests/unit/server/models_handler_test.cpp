#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "server/endpoints/models.hpp"
#include "server/error_writer.hpp"
#include "server/pool/account_pool.hpp"
#include "server/pool/affinity.hpp"
#include "server/pool/health.hpp"
#include "server/router.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
#include "support/temp_dir.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using qoderz::error_kind;
using qoderz::json;
using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::config::resolved_site;
using qoderz::qoder::auth::site_resolver;
using qoderz::qoder::auth::token_service;
using qoderz::qoder::catalog::catalog_service;
using qoderz::server::account_health;
using qoderz::server::account_pool;
using qoderz::server::failure_kind;
using qoderz::server::pool_account;
using qoderz::server::pool_options;
using qoderz::server::session_affinity;
using qoderz::server::endpoints::models_handler;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using qoderz::testing::temp_dir;

constexpr std::string_view site_id = "qoder";
constexpr std::string_view listing_path = "/algo/api/v2/model/list";
constexpr std::int64_t now_ms = 1'760'000'000'000;
constexpr std::int64_t one_hour_ms = 3600 * 1000;

std::string listing_of(std::vector<std::string> const& keys) {
  json app = json::array();
  for (auto const& key : keys) {
    app.push_back(json{{"key", key},
                       {"display_name", key + "-name"},
                       {"enable", true},
                       {"max_input_tokens", 200000},
                       {"is_vl", true}});
  }
  return json{{"app", std::move(app)}, {"chat", json::array()}}.dump();
}

struct listing_answer {
  int m_status = 200;
  std::string m_body;
};

struct upstream_state {
  std::map<std::string, listing_answer> m_answers;
  std::size_t m_listing_calls = 0;
};

struct fixture {
  temp_dir m_dir;
  credential_store m_store;
  fake_clock m_clock;
  account_health m_health{m_clock};
  session_affinity m_affinity;
  resolved_site m_site;
  asio::io_context m_io;
  upstream_state m_upstream_state;
  stub_server m_upstream;
  qoderz::net::http_client m_http;
  catalog_service m_catalog;
  token_service m_tokens;
  std::unique_ptr<account_pool> m_pool;

  explicit fixture(std::vector<std::string> names = {"ada"}, pool_options options = {})
      : m_store{m_dir.file("credentials.json")}, m_clock{now_ms},
        m_upstream{m_io,
                   [this](stub_request const& request) -> asio::awaitable<stub_response> {
                     if (qoderz::server::path_of(request.m_target) != listing_path) {
                       stub_response missing;
                       missing.m_status = 404;
                       co_return missing;
                     }
                     ++m_upstream_state.m_listing_calls;
                     auto const* uid = qoderz::net::header_value(request.m_headers, "Cosy-User");
                     auto const found =
                         m_upstream_state.m_answers.find(uid == nullptr ? std::string{} : *uid);
                     if (found == m_upstream_state.m_answers.end()) {
                       stub_response missing;
                       missing.m_status = 500;
                       missing.m_body = "unknown account";
                       co_return missing;
                     }
                     stub_response response;
                     response.m_status = found->second.m_status;
                     response.m_body = found->second.m_body;
                     co_return response;
                   }},
        m_http{m_io}, m_catalog{m_http, m_clock},
        m_tokens{m_http, m_store,
                 [this](std::string_view) -> qoderz::result<resolved_site> { return m_site; },
                 m_clock} {
    REQUIRE(m_upstream.start().has_value());
    m_site = loopback_site(m_upstream);
    std::vector<pool_account> accounts;
    credential_file file;
    for (auto const& name : names) {
      auto record = make_account(name);
      file.m_accounts.emplace(record.key(), record);
      accounts.push_back(
          pool_account{.m_key = record.key(), .m_account = record, .m_site = m_site});
    }
    REQUIRE(m_store.save(file).has_value());
    auto prepared = account_pool::prepare(accounts, options);
    REQUIRE(prepared.has_value());
    m_pool = std::make_unique<account_pool>(std::move(accounts), m_health, m_affinity,
                                            std::move(options), std::move(*prepared));
  }

  static resolved_site loopback_site(stub_server& server) {
    auto site = qoderz::config::find_site(site_id);
    REQUIRE(site.has_value());
    auto resolved = qoderz::config::resolve_site(**site, "");
    REQUIRE(resolved.has_value());
    auto overridden = qoderz::config::apply_loopback_override(*resolved, server.origin());
    REQUIRE(overridden.has_value());
    return std::move(*overridden);
  }

  static account_record make_account(std::string_view name) {
    account_record record;
    record.m_site = std::string{site_id};
    record.m_name = std::string{name};
    record.m_access_token = "test-access-token-not-real";
    record.m_refresh_token = "test-refresh-token-not-real";
    record.m_expires_at = now_ms + one_hour_ms;
    record.m_uid = "uid-" + std::string{name};
    record.m_display_name = std::string{name};
    record.m_machine_id = "machine-test-not-real";
    return record;
  }

  models_handler make_handler() { return models_handler{*m_pool, m_catalog, m_tokens, m_clock}; }

  void answer(std::string_view name, int status, std::string body) {
    m_upstream_state.m_answers.insert_or_assign(
        "uid-" + std::string{name}, listing_answer{.m_status = status, .m_body = std::move(body)});
  }

  void answer(std::string_view name, std::vector<std::string> keys) {
    answer(name, 200, listing_of(std::move(keys)));
  }

  [[nodiscard]] std::size_t listing_calls() const { return m_upstream_state.m_listing_calls; }
};

std::vector<std::string> ids_of(std::string const& body) {
  auto const parsed = json::parse(body, nullptr, false);
  REQUIRE(parsed.is_object());
  std::vector<std::string> ids;
  for (auto const& entry : parsed.at("data")) {
    ids.push_back(entry.at("id").get<std::string>());
  }
  return ids;
}

} // namespace

TEST_CASE("models merges the catalogues of two accounts", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", {"alpha", "shared"});
  scope.answer("bob", {"beta", "shared"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());

  CHECK(ids_of(**outcome) == std::vector<std::string>{"alpha", "shared", "beta"});
  auto const parsed = json::parse(**outcome, nullptr, false);
  CHECK(parsed.at("object") == "list");
  CHECK(parsed.at("data").size() == 3);
  CHECK(scope.listing_calls() == 2);
}

TEST_CASE("models deduplicates keys ignoring case, first account wins", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", {"FAST", "alpha"});
  scope.answer("bob", {"fast", "beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(ids_of(**outcome) == std::vector<std::string>{"FAST", "alpha", "beta"});
}

TEST_CASE("models writes the documented model fields", "[server][models]") {
  fixture scope{{"ada"}};
  scope.answer("ada", {"ultimate"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());

  auto const parsed = json::parse(**outcome, nullptr, false);
  auto const& entry = parsed.at("data").at(0);
  CHECK(entry.at("id") == "ultimate");
  CHECK(entry.at("object") == "model");
  CHECK(entry.at("owned_by") == "qoder");
  CHECK(entry.at("context_window") == 200000);
  CHECK(entry.at("supports_images") == true);
  CHECK(entry.at("supports_tools") == true);
  CHECK(entry.at("reasoning") == false);
  CHECK(entry.at("created") == now_ms / 1000);
  CHECK(entry.find("name") == entry.end());
}

TEST_CASE("models shares one created value across the list", "[server][models]") {
  fixture scope{{"ada"}};
  scope.answer("ada", {"alpha", "beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const parsed = json::parse(**outcome, nullptr, false);
  auto const first = parsed.at("data").at(0).at("created").get<std::int64_t>();
  auto const second = parsed.at("data").at(1).at("created").get<std::int64_t>();
  CHECK(first == second);
}

TEST_CASE("models serves the second request from the catalogue cache", "[server][models]") {
  fixture scope{{"ada"}};
  scope.answer("ada", {"alpha"});
  auto handler = scope.make_handler();

  REQUIRE(run_awaitable(scope.m_io, handler.build_json()).has_value());
  REQUIRE(run_awaitable(scope.m_io, handler.build_json()).has_value());
  CHECK(scope.listing_calls() == 1);
}

TEST_CASE("models cools an account down after an auth failure", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", 401, R"({"code":105,"message":"expired"})");
  scope.answer("bob", {"beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(ids_of(**outcome) == std::vector<std::string>{"beta"});

  auto const state = scope.m_health.state("qoder/ada");
  REQUIRE(state.has_value());
  CHECK(state->m_kind == failure_kind::auth);
  CHECK(scope.m_health.is_cooling("qoder/ada"));
}

TEST_CASE("models cools an account down after a quota failure", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", 429, R"({"code":110,"message":"quota exhausted"})");
  scope.answer("bob", {"beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const state = scope.m_health.state("qoder/ada");
  REQUIRE(state.has_value());
  CHECK(state->m_kind == failure_kind::quota);
  CHECK(scope.m_health.is_cooling("qoder/ada"));
}

TEST_CASE("models skips a 500 without cooling the account", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", 500, "boom");
  scope.answer("bob", {"beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(ids_of(**outcome) == std::vector<std::string>{"beta"});
  CHECK_FALSE(scope.m_health.is_cooling("qoder/ada"));
}

TEST_CASE("models reports an empty pool as a 401", "[server][models]") {
  fixture scope{{}};
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(qoderz::server::http_status_for(outcome->error()) == 401);
  CHECK(outcome->error().m_message == "not signed in; run: qoderz login");
}

TEST_CASE("models reports a fully cooled pool as 401 when every cooldown is auth",
          "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.m_health.report_failure("qoder/ada", failure_kind::auth);
  scope.m_health.report_failure("qoder/bob", failure_kind::auth);
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(qoderz::server::http_status_for(outcome->error()) == 401);
  CHECK(outcome->error().m_message ==
        "all Qoder accounts are temporarily unavailable; retry later");
  CHECK(scope.listing_calls() == 0);
}

TEST_CASE("models reports a fully cooled pool as 429 when a cooldown is quota",
          "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.m_health.report_failure("qoder/ada", failure_kind::quota);
  scope.m_health.report_failure("qoder/bob", failure_kind::auth);
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(qoderz::server::http_status_for(outcome->error()) == 429);
  CHECK(outcome->error().m_kind == error_kind::quota);
}

TEST_CASE("models answers a pinned pool with the pinned account only", "[server][models]") {
  pool_options options;
  options.m_pinned_key = "qoder/bob";
  fixture scope{{"ada", "bob"}, options};

  scope.answer("bob", {"beta"});
  auto handler = scope.make_handler();

  auto outcome = run_awaitable(scope.m_io, handler.build_json());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(ids_of(**outcome) == std::vector<std::string>{"beta"});
  CHECK(scope.listing_calls() == 1);
}

TEST_CASE("models does not move the session binding", "[server][models]") {
  fixture scope{{"ada", "bob"}};
  scope.answer("ada", {"alpha"});
  scope.answer("bob", {"beta"});
  auto handler = scope.make_handler();

  REQUIRE(run_awaitable(scope.m_io, handler.build_json()).has_value());
  CHECK_FALSE(scope.m_affinity.bound("models", scope.m_clock.now_ms()).has_value());
  CHECK(scope.m_affinity.size() == 0);
}
