// GCC 14 pairs Asio's awaitable-frame `std::aligned_alloc` with `std::free` and then warns about
// the mismatch, which is the allocation pair Asio itself uses and a legal one.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "openai/selection/session.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "qoder/chat/client.hpp"
#include "server/config.hpp"
#include "server/endpoints/chat.hpp"
#include "server/endpoints/models.hpp"
#include "server/pool/account_pool.hpp"
#include "server/pool/affinity.hpp"
#include "server/pool/health.hpp"
#include "server/router.hpp"
#include "server/server.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/server_harness.hpp"
#include "support/stub_server.hpp"
#include "support/temp_dir.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
using qoderz::openai::selection::session_store;
using qoderz::qoder::auth::token_service;
using qoderz::qoder::catalog::catalog_service;
using qoderz::qoder::chat::chat_client;
using qoderz::server::account_health;
using qoderz::server::account_pool;
using qoderz::server::pool_account;
using qoderz::server::server;
using qoderz::server::server_config;
using qoderz::server::server_dependencies;
using qoderz::server::session_affinity;
using qoderz::server::endpoints::chat_handler_impl;
using qoderz::server::endpoints::models_handler;
using qoderz::test::fake_clock;
using qoderz::test::header_of;
using qoderz::test::http_result;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using qoderz::testing::temp_dir;

constexpr std::string_view site_id = "qoder";
constexpr std::string_view listing_path = "/algo/api/v2/model/list";
constexpr std::string_view chat_path = "/algo/api/v2/service/pro/sse/agent_chat_generation";
constexpr std::int64_t now_ms = 1'760'000'000'000;
constexpr std::int64_t one_hour_ms = 3600 * 1000;

struct fixed_wrapper final : qoderz::protocol::rsa_key_wrapper {
  [[nodiscard]] qoderz::result<std::string> wrap(std::string_view) const override {
    return std::string(128, '\x5a');
  }
};

std::string listing_of(std::vector<std::string> const& keys) {
  json app = json::array();
  for (auto const& key : keys) {
    app.push_back(
        json{{"key", key}, {"display_name", key}, {"enable", true}, {"max_input_tokens", 1000000}});
  }
  return json{{"app", std::move(app)}, {"chat", json::array()}}.dump();
}

std::string envelope(std::string_view body) {
  std::string line = R"({"statusCodeValue":200,"body":")";
  for (char const character : body) {
    if (character == '"' || character == '\\') {
      line.push_back('\\');
    }
    line.push_back(character);
  }
  line.append("\"}");
  return line;
}

std::string frame(std::string_view body) {
  return "data: " + envelope(body) + "\n";
}

std::string success_stream() {
  std::string body;
  body.append(frame(R"({"choices":[{"index":0,"delta":{"content":"Hello"}}]})"));
  body.append(
      frame(R"({"choices":[{"index":0,"delta":{"content":" world"},"finish_reason":"stop"}]})"));
  body.append("data: ").append(envelope("[DONE]")).append("\n");
  return body;
}

struct upstream_state {
  std::map<std::string, int> m_listing_status;
  std::map<std::string, std::string> m_listing_body;
  std::map<std::string, int> m_chat_status;
  std::map<std::string, std::string> m_chat_body;
  std::vector<stub_request> m_chat_requests;
  std::size_t m_listing_calls = 0;
};

/// A whole `qoderz serve` over a loopback stub: real listener, real router, real chat handler.
struct fixture {
  temp_dir m_dir;
  credential_store m_store;
  fake_clock m_clock;
  account_health m_health{m_clock};
  session_affinity m_affinity;
  asio::io_context m_io;
  upstream_state m_upstream_state;
  stub_server m_upstream;
  resolved_site m_site;
  resolved_site m_public_site;
  qoderz::net::http_client m_http;
  catalog_service m_catalog;
  token_service m_tokens;
  fixed_wrapper m_wrapper;
  chat_client m_client;
  session_store m_sessions;
  std::unique_ptr<account_pool> m_pool;
  std::unique_ptr<models_handler> m_models;
  std::unique_ptr<chat_handler_impl> m_chat;
  std::unique_ptr<server> m_server;
  std::uint16_t m_port = 0;

  explicit fixture(std::vector<std::string> names = {"ada"},
                   qoderz::server::pool_options options = {},
                   std::string site = std::string{site_id}, std::string vpc = {})
      : m_store{m_dir.file("credentials.json")}, m_clock{now_ms},
        m_upstream{m_io,
                   [this](stub_request const& request) -> asio::awaitable<stub_response> {
                     co_return answer(request);
                   }},
        m_http{m_io}, m_catalog{m_http, m_clock},
        m_tokens{m_http, m_store,
                 [this](std::string_view instance) { return resolve_site(instance); }, m_clock},
        m_client{m_http, m_wrapper, m_clock,
                 qoderz::qoder::chat::retry_options{.m_total_budget = std::chrono::milliseconds{0},
                                                    .m_max_attempts = 1,
                                                    .m_min_delay = std::chrono::milliseconds{0},
                                                    .m_max_delay = std::chrono::milliseconds{0}}} {
    REQUIRE(m_upstream.start().has_value());
    auto definition = qoderz::config::find_site(site);
    REQUIRE(definition.has_value());
    auto resolved = qoderz::config::resolve_site(**definition, vpc);
    REQUIRE(resolved.has_value());
    m_public_site = *resolved;
    auto overridden = qoderz::config::apply_loopback_override(*resolved, m_upstream.origin());
    REQUIRE(overridden.has_value());
    m_site = std::move(*overridden);

    credential_file file;
    std::vector<pool_account> accounts;
    for (auto const& name : names) {
      auto record = make_account(name);
      record.m_site = m_public_site.m_site_id;
      record.m_vpc = vpc;
      file.m_accounts.emplace(record.key(), record);
      accounts.push_back(
          pool_account{.m_key = record.key(), .m_account = record, .m_site = m_site});
    }
    REQUIRE(m_store.save(file).has_value());

    auto prepared = account_pool::prepare(accounts, options);
    REQUIRE(prepared.has_value());
    m_pool = std::make_unique<account_pool>(std::move(accounts), m_health, m_affinity,
                                            std::move(options), std::move(*prepared));
    m_models = std::make_unique<models_handler>(*m_pool, m_catalog, m_tokens, m_clock);
    m_chat = std::make_unique<chat_handler_impl>(*m_pool, m_catalog, m_tokens, m_client, m_sessions,
                                                 m_clock, m_public_site.m_site_id);

    server_config config;
    config.m_listen_host = "127.0.0.1";
    config.m_port = 0;
    m_server = std::make_unique<server>(
        m_io, std::move(config), server_dependencies{.m_chat = *m_chat, .m_models = *m_models});
    auto started = m_server->start();
    REQUIRE(started.has_value());
    m_port = *started;
    asio::co_spawn(m_io, m_server->run(), asio::detached);
  }

  ~fixture() {
    if (m_server) {
      m_server->stop();
    }
  }

  fixture(fixture const&) = delete;
  fixture(fixture&&) = delete;
  fixture& operator=(fixture const&) = delete;
  fixture& operator=(fixture&&) = delete;

  [[nodiscard]] std::string const& site_name() const { return m_public_site.m_site_id; }

  [[nodiscard]] qoderz::result<resolved_site> resolve_site(std::string_view instance) const {
    auto definition = qoderz::config::find_site(m_public_site.m_site_id);
    REQUIRE(definition.has_value());
    auto resolved = qoderz::config::resolve_site(**definition, instance);
    if (!resolved) {
      return std::unexpected(std::move(resolved.error()));
    }
    return qoderz::config::apply_loopback_override(*resolved, m_upstream.origin());
  }

  [[nodiscard]] static account_record make_account(std::string_view name) {
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

  [[nodiscard]] stub_response answer(stub_request const& request) {
    auto const path = qoderz::server::path_of(request.m_target);
    auto const* uid = qoderz::net::header_value(request.m_headers, "Cosy-User");
    auto const name = uid == nullptr ? std::string{} : *uid;

    stub_response response;
    if (path == listing_path) {
      ++m_upstream_state.m_listing_calls;
      response.m_status = 200;
      response.m_body = listing_of({"ultimate"});
      if (auto const found = m_upstream_state.m_listing_status.find(name);
          found != m_upstream_state.m_listing_status.end()) {
        response.m_status = found->second;
      }
      if (auto const found = m_upstream_state.m_listing_body.find(name);
          found != m_upstream_state.m_listing_body.end()) {
        response.m_body = found->second;
      }
      return response;
    }
    if (path == chat_path) {
      m_upstream_state.m_chat_requests.push_back(request);
      response.m_status = 200;
      response.m_body = success_stream();
      response.m_headers.push_back(
          qoderz::net::http_header{.m_name = "Content-Type", .m_value = "text/event-stream"});
      if (auto const found = m_upstream_state.m_chat_status.find(name);
          found != m_upstream_state.m_chat_status.end()) {
        response.m_status = found->second;
      }
      if (auto const found = m_upstream_state.m_chat_body.find(name);
          found != m_upstream_state.m_chat_body.end()) {
        response.m_body = found->second;
      }
      return response;
    }
    response.m_status = 404;
    return response;
  }

  void listing(std::string_view name, std::vector<std::string> keys) {
    m_upstream_state.m_listing_body.insert_or_assign("uid-" + std::string{name},
                                                     listing_of(std::move(keys)));
  }

  void listing_status(std::string_view name, int status) {
    m_upstream_state.m_listing_status.insert_or_assign("uid-" + std::string{name}, status);
  }

  void chat(std::string_view name, int status, std::string body) {
    m_upstream_state.m_chat_status.insert_or_assign("uid-" + std::string{name}, status);
    m_upstream_state.m_chat_body.insert_or_assign("uid-" + std::string{name}, std::move(body));
  }

  [[nodiscard]] std::optional<http_result>
  chat_request(std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) {
    auto wire = qoderz::test::build_request("POST", "/v1/chat/completions", std::move(headers),
                                            body, false);
    auto nested = run_awaitable(m_io, qoderz::test::one_request(m_port, std::move(wire)));
    if (!nested.has_value()) {
      return std::nullopt;
    }
    return std::move(*nested);
  }

  [[nodiscard]] std::size_t chat_calls() const { return m_upstream_state.m_chat_requests.size(); }
};

std::string body_of(std::string_view model, bool stream) {
  json body = json::object();
  body["model"] = model;
  body["stream"] = stream;
  body["messages"] = json::array({json{{"role", "user"}, {"content", "Hello"}}});
  return body.dump();
}

json error_of(http_result const& result) {
  return json::parse(result.m_body, nullptr, false).at("error");
}

} // namespace

TEST_CASE("the endpoint streams a chat answer over HTTP", "[server][chat][integration]") {
  fixture scope;
  auto outcome = scope.chat_request(body_of("ultimate", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(header_of(*outcome, "Content-Type") == "text/event-stream");
  CHECK(header_of(*outcome, "Cache-Control") == "no-cache");
  CHECK(header_of(*outcome, "X-Accel-Buffering") == "no");
  CHECK(outcome->m_chunked);
  CHECK(outcome->m_body.find(R"("content":"Hello")") != std::string::npos);
  CHECK(outcome->m_body.ends_with("data: [DONE]\n\n"));
}

TEST_CASE("the endpoint answers a non-streaming request with one JSON body",
          "[server][chat][integration]") {
  fixture scope;
  auto outcome = scope.chat_request(body_of("ultimate", false));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(header_of(*outcome, "Content-Type") == "application/json");
  auto const document = json::parse(outcome->m_body, nullptr, false);
  CHECK(document.at("object") == "chat.completion");
  CHECK(document.at("choices").at(0).at("message").at("content") == "Hello world");
}

TEST_CASE("a quota account fails over inside one HTTP request", "[server][chat][integration]") {
  fixture scope{{"ada", "bob"}};
  scope.chat("ada", 429, R"({"code":110,"message":"quota exhausted"})");
  auto outcome = scope.chat_request(body_of("ultimate", true), {{"X-Session-Id", "session-a"}});
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(scope.chat_calls() == 2);
  CHECK(outcome->m_body.ends_with("data: [DONE]\n\n"));
}

TEST_CASE("a model only one account offers is served by that account",
          "[server][chat][integration]") {
  fixture scope{{"ada", "bob"}};
  scope.listing("ada", {"alpha"});
  scope.listing("bob", {"ultimate"});
  auto outcome = scope.chat_request(body_of("ultimate", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  REQUIRE(scope.chat_calls() == 1);
  auto const* uid = qoderz::net::header_value(
      scope.m_upstream_state.m_chat_requests.at(0).m_headers, "Cosy-User");
  REQUIRE(uid != nullptr);
  CHECK(*uid == "uid-bob");
}

TEST_CASE("an unknown model is a 400 with the documented message", "[server][chat][integration]") {
  fixture scope;
  auto outcome = scope.chat_request(body_of("ghost", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 400);
  auto const failure = error_of(*outcome);
  CHECK(failure.at("message") == R"(unknown or disabled model "ghost")");
  CHECK(failure.at("type") == "invalid_request_error");
  CHECK(failure.at("code") == 400);
  CHECK(scope.chat_calls() == 0);
}

TEST_CASE("a broken request body never reaches an account", "[server][chat][integration]") {
  fixture scope;

  auto invalid = scope.chat_request("{");
  REQUIRE(invalid.has_value());
  CHECK(invalid->m_status == 400);
  CHECK(error_of(*invalid).at("message") == "the request body is not valid JSON");

  auto no_messages = scope.chat_request(R"({"model":"ultimate"})");
  REQUIRE(no_messages.has_value());
  CHECK(no_messages->m_status == 400);
  CHECK(error_of(*no_messages).at("message") == "messages is required");

  auto no_model = scope.chat_request(R"({"messages":[]})");
  REQUIRE(no_model.has_value());
  CHECK(no_model->m_status == 400);
  CHECK(error_of(*no_model).at("message") == "model is required");

  CHECK(scope.chat_calls() == 0);
}

TEST_CASE("an error before the first frame is a real HTTP status", "[server][chat][integration]") {
  fixture scope;
  scope.listing_status("ada", 429);
  auto outcome = scope.chat_request(body_of("ultimate", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 429);
  CHECK(error_of(*outcome).at("type") == "insufficient_quota");
  CHECK(outcome->m_body.find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("a pinned pool never touches the other account", "[server][chat][integration]") {
  qoderz::server::pool_options options;
  options.m_pinned_key = "qoder/ada";
  fixture scope{{"ada", "bob"}, std::move(options)};
  scope.chat("ada", 429, R"({"code":110,"message":"quota exhausted"})");

  auto outcome = scope.chat_request(body_of("ultimate", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 429);
  CHECK(scope.chat_calls() == 1);
  auto const* uid = qoderz::net::header_value(
      scope.m_upstream_state.m_chat_requests.at(0).m_headers, "Cosy-User");
  REQUIRE(uid != nullptr);
  CHECK(*uid == "uid-ada");
  CHECK(scope.m_upstream_state.m_listing_calls == 1);
}

TEST_CASE("a VPC account reaches both endpoints through its own gateway host",
          "[server][chat][integration]") {
  fixture scope{{"ada"}, {}, "qoder-cn", "acme"};
  REQUIRE(scope.site_name() == "qoder-cn");
  CHECK(scope.m_site.m_vpc == "acme");

  // The site the account resolves to really is the VPC one; the loopback override only replaces the
  // origin, it does not drop the instance.
  CHECK(scope.m_public_site.m_api_base == "https://acme-gateway.vpc.qoder.com.cn");
  CHECK(scope.m_public_site.m_openapi_base == "https://acme-openapi.vpc.qoder.com.cn");

  auto outcome = scope.chat_request(body_of("qoder-cn/ultimate", true));
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(scope.m_upstream_state.m_listing_calls == 1);
  REQUIRE(scope.chat_calls() == 1);
  auto const& sent = scope.m_upstream_state.m_chat_requests.at(0);
  CHECK(sent.m_target.starts_with(chat_path));
  // A header count would hold for any request; the account that served it is what the VPC site
  // must carry through the loopback override.
  auto const* uid = qoderz::net::header_value(sent.m_headers, "Cosy-User");
  REQUIRE(uid != nullptr);
  CHECK(*uid == "uid-ada");
}
