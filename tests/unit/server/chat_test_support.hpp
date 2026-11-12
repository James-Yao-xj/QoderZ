#pragma once

#include "common/error.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "openai/selection/session.hpp"
#include "protocol/body_codec.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "qoder/chat/client.hpp"
#include "server/endpoints/chat.hpp"
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

/// Fixtures shared by the chat endpoint tests, so each case file reads as the behaviour it checks.
namespace qoderz::test::chat {

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
using qoderz::server::chat_http_request;
using qoderz::server::failure_kind;
using qoderz::server::pool_account;
using qoderz::server::pool_options;
using qoderz::server::response_sink;
using qoderz::server::session_affinity;
using qoderz::server::endpoints::chat_handler_impl;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using qoderz::testing::temp_dir;

inline constexpr std::string_view site_id = "qoder";
inline constexpr std::string_view listing_path = "/algo/api/v2/model/list";
inline constexpr std::string_view chat_path = "/algo/api/v2/service/pro/sse/agent_chat_generation";
inline constexpr std::int64_t now_ms = 1'760'000'000'000;
inline constexpr std::int64_t one_hour_ms = 3600 * 1000;

/// A wrapper with a fixed ciphertext: RSA wrapping is pinned by the COSY tests, not here.
struct fixed_wrapper final : qoderz::protocol::rsa_key_wrapper {
  [[nodiscard]] qoderz::result<std::string> wrap(std::string_view) const override {
    return std::string(128, '\x5a');
  }
};

inline std::string listing_of(std::vector<std::string> const& keys) {
  json app = json::array();
  for (auto const& key : keys) {
    app.push_back(
        json{{"key", key}, {"display_name", key}, {"enable", true}, {"max_input_tokens", 1000000}});
  }
  return json{{"app", std::move(app)}, {"chat", json::array()}}.dump();
}

/// One `data:` line carrying `body` behind the envelope's status code.
inline std::string envelope(int status, std::string_view body) {
  std::string line = "data: {\"statusCodeValue\":" + std::to_string(status) + ",\"body\":\"";
  for (char const character : body) {
    if (character == '"' || character == '\\') {
      line.push_back('\\');
    }
    line.push_back(character);
  }
  line.append("\"}\n");
  return line;
}

inline std::string frame(std::string_view body) {
  return envelope(200, body);
}

inline std::string done_frame() {
  return envelope(200, "[DONE]");
}

inline std::string success_stream() {
  std::string body = frame(R"({"choices":[{"index":0,"delta":{"content":"Hello"}}]})");
  body.append(
      frame(R"({"choices":[{"index":0,"delta":{"content":" world"},"finish_reason":"stop"}]})"));
  body.append(done_frame());
  return body;
}

struct upstream_state {
  std::map<std::string, int> m_listing_status;
  std::map<std::string, std::string> m_listing_body;
  std::map<std::string, int> m_chat_status;
  std::map<std::string, std::string> m_chat_body;
  std::map<std::string, bool> m_chat_truncate;
  std::vector<stub_request> m_chat_requests;
  std::size_t m_listing_calls = 0;
};

/// A sink that records what the handler wrote, so a case can assert on the whole answer.
class recording_sink final : public response_sink {
public:
  [[nodiscard]] bool started() const noexcept override { return m_started; }

  asio::awaitable<qoderz::result<void>> write_json(int status, std::string body) override {
    m_json_status = status;
    m_json_body = std::move(body);
    ++m_json_writes;
    m_started = true;
    co_return qoderz::result<void>{};
  }

  asio::awaitable<qoderz::result<void>> begin_sse() override {
    if (m_begin_failure.has_value()) {
      co_return std::unexpected(*m_begin_failure);
    }
    m_started = true;
    m_streaming = true;
    co_return qoderz::result<void>{};
  }

  asio::awaitable<qoderz::result<void>> write_sse(std::string_view frame_text) override {
    if (m_frames.size() >= m_break_after) {
      co_return std::unexpected(qoderz::make_error(error_kind::canceled, "the client went away"));
    }
    m_frames.emplace_back(frame_text);
    co_return qoderz::result<void>{};
  }

  asio::awaitable<qoderz::result<void>> end_sse() override {
    if (m_end_failure.has_value()) {
      co_return std::unexpected(*m_end_failure);
    }
    m_ended = true;
    co_return qoderz::result<void>{};
  }

  int m_json_status = 0;
  /// How many complete JSON bodies were written: more than one is a double answer.
  std::size_t m_json_writes = 0;
  std::string m_json_body;
  std::vector<std::string> m_frames;
  bool m_started = false;
  bool m_streaming = false;
  bool m_ended = false;
  /// Forces `begin_sse()` to fail the way a dead socket does, so a case can check that nothing is
  /// written after a head that never went out.
  std::optional<qoderz::error> m_begin_failure;
  /// Forces `end_sse()` to fail the way a socket that died between the Stop frame and the
  /// terminating chunk does, so a case can check that the turn is left unanswered.
  std::optional<qoderz::error> m_end_failure;
  std::size_t m_break_after = static_cast<std::size_t>(-1);
};

/// One chat handler over a loopback stub, with the pool, catalogue and token service it needs.
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
  std::string m_site_name;
  qoderz::net::http_client m_http;
  catalog_service m_catalog;
  token_service m_tokens;
  fixed_wrapper m_wrapper;
  chat_client m_client;
  session_store m_sessions;
  std::unique_ptr<account_pool> m_pool;

  explicit fixture(std::vector<std::string> names = {"ada"}, std::string vpc = {},
                   bool fresh = true, std::string site = std::string{site_id},
                   pool_options options = {})
      : m_store{m_dir.file("credentials.json")}, m_clock{now_ms},
        m_upstream{m_io,
                   [this](stub_request const& request) -> asio::awaitable<stub_response> {
                     co_return answer(request);
                   }},
        m_site_name{std::move(site)}, m_http{m_io}, m_catalog{m_http, m_clock},
        m_tokens{m_http, m_store, [this](std::string_view instance) { return resolve(instance); },
                 m_clock},
        m_client{m_http, m_wrapper, m_clock,
                 qoderz::qoder::chat::retry_options{.m_total_budget = std::chrono::milliseconds{0},
                                                    .m_max_attempts = 1,
                                                    .m_min_delay = std::chrono::milliseconds{0},
                                                    .m_max_delay = std::chrono::milliseconds{0}}} {
    REQUIRE(m_upstream.start().has_value());

    credential_file file;
    std::vector<pool_account> accounts;
    for (auto const& name : names) {
      auto record = make_account(name, vpc, fresh);
      file.m_accounts.emplace(record.key(), record);
      auto definition = qoderz::config::find_site(m_site_name);
      REQUIRE(definition.has_value());
      auto resolved = qoderz::config::resolve_site(**definition, vpc);
      REQUIRE(resolved.has_value());
      auto overridden = qoderz::config::apply_loopback_override(*resolved, m_upstream.origin());
      REQUIRE(overridden.has_value());
      if (m_site.m_site_id.empty()) {
        m_site = *resolved;
      }
      accounts.push_back(pool_account{
          .m_key = record.key(), .m_account = record, .m_site = std::move(*overridden)});
    }
    REQUIRE(m_store.save(file).has_value());

    auto prepared = account_pool::prepare(accounts, options);
    REQUIRE(prepared.has_value());
    m_pool = std::make_unique<account_pool>(std::move(accounts), m_health, m_affinity,
                                            std::move(options), std::move(*prepared));
  }

  fixture(fixture const&) = delete;
  fixture(fixture&&) = delete;
  fixture& operator=(fixture const&) = delete;
  fixture& operator=(fixture&&) = delete;
  ~fixture() = default;

  [[nodiscard]] qoderz::result<resolved_site> resolve(std::string_view instance) const {
    if (instance == "invalid") {
      return std::unexpected(qoderz::configuration_error("the VPC instance is not usable"));
    }
    auto definition = qoderz::config::find_site(m_site_name);
    REQUIRE(definition.has_value());
    auto resolved = qoderz::config::resolve_site(**definition, instance);
    REQUIRE(resolved.has_value());
    return qoderz::config::apply_loopback_override(*resolved, m_upstream.origin());
  }

  [[nodiscard]] static account_record make_account(std::string_view name, std::string_view vpc,
                                                   bool fresh) {
    account_record record;
    record.m_site = std::string{site_id};
    record.m_name = std::string{name};
    record.m_vpc = std::string{vpc};
    record.m_access_token = "test-access-token-not-real";
    record.m_refresh_token = "test-refresh-token-not-real";
    record.m_expires_at = fresh ? now_ms + one_hour_ms : now_ms - one_hour_ms;
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
      response.m_body = listing_of({"ultimate", "alpha"});
      pick(m_upstream_state.m_listing_status, name, response.m_status);
      pick(m_upstream_state.m_listing_body, name, response.m_body);
      return response;
    }
    if (path == chat_path) {
      m_upstream_state.m_chat_requests.push_back(request);
      response.m_body = success_stream();
      response.m_headers.push_back(
          qoderz::net::http_header{.m_name = "Content-Type", .m_value = "text/event-stream"});
      pick(m_upstream_state.m_chat_status, name, response.m_status);
      pick(m_upstream_state.m_chat_body, name, response.m_body);
      pick(m_upstream_state.m_chat_truncate, name, response.m_truncate_body);
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

  void truncate_chat(std::string_view name) {
    m_upstream_state.m_chat_truncate.insert_or_assign("uid-" + std::string{name}, true);
  }

  [[nodiscard]] chat_handler_impl make_handler() {
    return chat_handler_impl{*m_pool,    m_catalog, m_tokens,   m_client,
                             m_sessions, m_clock,   m_site_name};
  }

  [[nodiscard]] std::size_t chat_calls() const { return m_upstream_state.m_chat_requests.size(); }
  [[nodiscard]] std::size_t listing_calls() const { return m_upstream_state.m_listing_calls; }

  /// Which account served chat request `index`: the upstream sees only the credential's uid, so
  /// that is the pool identity a case can assert against.
  [[nodiscard]] std::string account_of(std::size_t index) const {
    auto const& request = m_upstream_state.m_chat_requests.at(index);
    auto const* uid = qoderz::net::header_value(request.m_headers, "Cosy-User");
    return uid == nullptr ? std::string{} : std::string{*uid};
  }

  /// The pool key of the account that served chat request `index`: the stub identifies an
  /// account by its credential uid, and the fixture's uids are `uid-` plus the account name.
  [[nodiscard]] std::string served_key(std::size_t index) const {
    auto const uid = account_of(index);
    return uid.starts_with("uid-") ? m_pool->site_id() + "/" + uid.substr(4) : uid;
  }

  /// The affinity key of a request that carries only a header: §6.3 keys the table by the derived
  /// session id of §4.1, and that branch of the derivation ignores every prompt field.
  [[nodiscard]] std::string session_key(std::string_view header) const {
    return m_sessions.make_session_id(header, std::nullopt, std::nullopt, "", "");
  }

  /// The account `header` is bound to, looked up by its derived id rather than by the header.
  [[nodiscard]] std::optional<std::string> bound_account(std::string_view header) {
    return m_affinity.bound(session_key(header), m_clock.now_ms());
  }

  [[nodiscard]] json sent_body(std::size_t index) const {
    auto plain = qoderz::protocol::body_decode(m_upstream_state.m_chat_requests.at(index).m_body);
    REQUIRE(plain.has_value());
    return json::parse(*plain, nullptr, false);
  }

private:
  template <class value_type>
  static void pick(std::map<std::string, value_type> const& table, std::string const& name,
                   value_type& out) {
    if (auto const found = table.find(name); found != table.end()) {
      out = found->second;
    }
  }
};

/// The smallest request the endpoint accepts: one user turn and the stream flag it reads.
inline std::string body_of(std::string_view model, bool stream, std::string_view prompt = "Hello") {
  return json{{"model", model},
              {"stream", stream},
              {"messages", json::array({json{{"role", "user"}, {"content", prompt}}})}}
      .dump();
}

inline std::string payload_of(std::string const& written) {
  auto text = written;
  if (text.starts_with("data: ")) {
    text.erase(0, 6);
  }
  while (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  return text;
}

inline json chunk_of(std::string const& written) {
  return json::parse(payload_of(written), nullptr, false);
}

/// What one `handle` call produced: the outcome the router would see and everything it wrote.
///
/// The sink is held behind a `unique_ptr` because `response_sink` is neither copyable nor movable,
/// and the case wants `auto done = ask(...)` rather than an out parameter.
struct exchange {
  exchange() : m_sink{std::make_unique<recording_sink>()} {}
  exchange(exchange&&) noexcept = default;
  exchange& operator=(exchange&&) noexcept = default;
  exchange(exchange const&) = delete;
  exchange& operator=(exchange const&) = delete;
  ~exchange() = default;

  recording_sink& sink() { return *m_sink; }
  recording_sink const& sink() const { return *m_sink; }

  qoderz::result<void> m_outcome;
  std::unique_ptr<recording_sink> m_sink;
};

/// Runs one body through a fresh handler and keeps everything the sink recorded.
inline exchange deliver(fixture& scope, std::string body, std::string session = {}) {
  auto handler = scope.make_handler();
  chat_http_request request;
  request.m_body = std::move(body);
  request.m_session_id = std::move(session);
  request.m_stream = true;

  exchange done;
  auto outcome = run_awaitable(scope.m_io, handler.handle(request, *done.m_sink));
  REQUIRE(outcome.has_value());
  done.m_outcome = std::move(*outcome);
  return done;
}

inline exchange ask(fixture& scope, std::string_view model, bool stream, std::string session = {}) {
  return deliver(scope, body_of(model, stream), std::move(session));
}

/// The same, with another first user turn: the fallback branch of §4.1 folds it into the id.
inline exchange ask_prompt(fixture& scope, std::string_view model, bool stream,
                           std::string_view prompt, std::string session = {}) {
  return deliver(scope, body_of(model, stream, prompt), std::move(session));
}

inline void check_no_done_frame(std::vector<std::string> const& frames) {
  for (auto const& written : frames) {
    CHECK(written.find("[DONE]") == std::string::npos);
  }
}

} // namespace qoderz::test::chat
