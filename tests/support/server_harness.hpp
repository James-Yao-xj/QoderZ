#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "common/strings.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "server/config.hpp"
#include "server/endpoints/models.hpp"
#include "server/http/chat_handler.hpp"
#include "server/http/response_sink.hpp"
#include "server/pool/account_pool.hpp"
#include "server/pool/affinity.hpp"
#include "server/pool/health.hpp"
#include "server/router.hpp"
#include "server/server.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
#include "support/temp_dir.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::config::resolved_site;
using qoderz::qoder::auth::token_service;
using qoderz::qoder::catalog::catalog_service;
using qoderz::server::account_health;
using qoderz::server::account_pool;
using qoderz::server::chat_handler;
using qoderz::server::chat_http_request;
using qoderz::server::pool_account;
using qoderz::server::pool_options;
using qoderz::server::response_sink;
using qoderz::server::server;
using qoderz::server::server_config;
using qoderz::server::server_dependencies;
using qoderz::server::session_affinity;
using qoderz::server::endpoints::models_handler;
using qoderz::testing::temp_dir;

inline constexpr std::string_view fake_key = "test-api-key-not-real";
inline constexpr std::string_view site_id = "qoder";
inline constexpr std::string_view listing_path = "/algo/api/v2/model/list";
inline constexpr std::int64_t now_ms = 1'760'000'000'000;
inline constexpr std::int64_t one_hour_ms = 3600 * 1000;

struct http_result {
  int m_status = 0;
  std::map<std::string, std::string> m_headers;
  std::string m_body;
  bool m_chunked = false;
};

inline std::string header_of(http_result const& result, std::string_view name) {
  for (auto const& [key, value] : result.m_headers) {
    if (qoderz::strings::equals_ignore_case(key, name)) {
      return value;
    }
  }
  return {};
}

inline std::string build_request(std::string_view method, std::string_view target,
                                 std::vector<std::pair<std::string, std::string>> const& headers,
                                 std::string_view body, bool keep_alive) {
  std::string wire;
  wire.append(method);
  wire.append(" ");
  wire.append(target);
  wire.append(" HTTP/1.1\r\nHost: 127.0.0.1\r\n");
  for (auto const& [name, value] : headers) {
    wire.append(name);
    wire.append(": ");
    wire.append(value);
    wire.append("\r\n");
  }
  if (!body.empty() || method == "POST") {
    wire.append("Content-Length: ");
    wire.append(std::to_string(body.size()));
    wire.append("\r\n");
  }
  wire.append(keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
  wire.append("\r\n");
  wire.append(body);
  return wire;
}

/// Writes one request and reads exactly one response. A write error is not fatal on its own: the
/// 413 path closes the socket while the client is still writing, and the answer is already in the
/// receive buffer.
inline asio::awaitable<std::optional<http_result>>
exchange(beast::tcp_stream& stream, std::string wire, beast::flat_buffer& buffer) {
  auto [write_ec, written] = co_await asio::async_write(stream.socket(), asio::buffer(wire),
                                                        asio::as_tuple(asio::use_awaitable));
  (void)write_ec;
  (void)written;
  http::response<http::string_body> response;
  auto [read_ec, read_bytes] =
      co_await http::async_read(stream, buffer, response, asio::as_tuple(asio::use_awaitable));
  (void)read_bytes;
  if (read_ec) {
    co_return std::nullopt;
  }
  http_result result;
  result.m_status = response.result_int();
  for (auto const& field : response.base()) {
    result.m_headers.emplace(std::string{field.name_string()}, std::string{field.value()});
  }
  result.m_body = response.body();
  result.m_chunked = response.chunked();
  co_return result;
}

inline asio::awaitable<std::optional<http_result>> one_request(std::uint16_t port,
                                                               std::string wire) {
  auto executor = co_await asio::this_coro::executor;
  beast::tcp_stream stream{executor};
  beast::error_code ec;
  stream.connect(tcp::endpoint{asio::ip::address_v4::loopback(), port}, ec);
  if (ec) {
    co_return std::nullopt;
  }
  beast::flat_buffer buffer;
  auto result = co_await exchange(stream, std::move(wire), buffer);
  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  co_return result;
}

/// A chat handler that answers in a fixed shape, so the HTTP plumbing is observed on its own.
class fake_chat final : public chat_handler {
public:
  enum class mode {
    json_ok,
    sse_ok,
    slow_json,
    error_before_start,
    error_after_start,
    error_quota,
    error_busy,
    error_canceled
  };

  explicit fake_chat(mode how, std::chrono::milliseconds delay = std::chrono::milliseconds{0})
      : m_mode{how}, m_delay{delay} {}

  asio::awaitable<qoderz::result<void>> handle(chat_http_request const& request,
                                               response_sink& sink) override {
    m_last = request;
    ++m_calls;
    switch (m_mode) {
    case mode::slow_json: {
      asio::steady_timer timer{co_await asio::this_coro::executor, m_delay};
      auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
      (void)ec;
      co_return co_await sink.write_json(200, R"({"object":"chat.completion","ok":true})");
    }
    case mode::json_ok:
      co_return co_await sink.write_json(200, R"({"object":"chat.completion","ok":true})");
    case mode::sse_ok: {
      auto begun = co_await sink.begin_sse();
      if (!begun) {
        co_return begun;
      }
      auto first = co_await sink.write_sse("data: {\"n\":1}\n\n");
      if (!first) {
        co_return first;
      }
      auto last = co_await sink.write_sse("data: [DONE]\n\n");
      if (!last) {
        co_return last;
      }
      co_return co_await sink.end_sse();
    }
    case mode::error_before_start:
      co_return std::unexpected(qoderz::bad_request_error("messages is required"));
    case mode::error_quota:
      co_return std::unexpected(qoderz::quota_error("every account is cooling", {}, 30));
    case mode::error_busy: {
      auto busy = qoderz::make_error(qoderz::error_kind::busy, "the queue is full");
      busy.m_retry_after = 7;
      co_return std::unexpected(std::move(busy));
    }
    case mode::error_canceled:
      co_return std::unexpected(qoderz::make_error(qoderz::error_kind::canceled, "client gone"));
    case mode::error_after_start: {
      auto begun = co_await sink.begin_sse();
      if (!begun) {
        co_return begun;
      }
      auto frame = co_await sink.write_sse("data: {\"n\":1}\n\n");
      if (!frame) {
        co_return frame;
      }
      co_return std::unexpected(qoderz::upstream_error("upstream exploded"));
    }
    }
    co_return qoderz::result<void>{};
  }

  chat_http_request m_last;
  std::size_t m_calls = 0;

private:
  mode m_mode;
  std::chrono::milliseconds m_delay;
};

/// A real listener on 127.0.0.1:0, a real models handler over a loopback stub and a fake chat
/// handler. Nothing here can reach a non-loopback address.
struct server_harness {
  temp_dir m_dir;
  credential_store m_store;
  fake_clock m_clock;
  asio::io_context m_io;
  int m_upstream_status = 200;
  std::size_t m_listing_calls = 0;
  stub_server m_upstream;
  resolved_site m_site;
  qoderz::net::http_client m_http;
  catalog_service m_catalog;
  token_service m_tokens;
  account_health m_health{m_clock};
  session_affinity m_affinity;
  std::unique_ptr<account_pool> m_pool;
  std::optional<models_handler> m_models;

  fake_chat m_chat;
  std::optional<server> m_server;
  std::uint16_t m_port = 0;

  explicit server_harness(fake_chat::mode how = fake_chat::mode::json_ok,
                          std::optional<std::string> key = {},
                          std::size_t max_body_bytes = 32 * 1024 * 1024,
                          std::chrono::milliseconds chat_delay = std::chrono::milliseconds{0})
      : m_store{m_dir.file("credentials.json")}, m_clock{now_ms},
        m_upstream{m_io,
                   [this](stub_request const& request) -> asio::awaitable<stub_response> {
                     if (qoderz::server::path_of(request.m_target) != listing_path) {
                       stub_response missing;
                       missing.m_status = 404;
                       co_return missing;
                     }
                     ++m_listing_calls;
                     stub_response response;
                     response.m_status = m_upstream_status;
                     response.m_body = m_listing_body;
                     co_return response;
                   }},
        m_http{m_io}, m_catalog{m_http, m_clock},
        m_tokens{m_http, m_store,
                 [this](std::string_view) -> qoderz::result<resolved_site> { return m_site; },
                 m_clock},
        m_chat{how, chat_delay} {
    REQUIRE(m_upstream.start().has_value());
    auto site = qoderz::config::find_site(site_id);
    REQUIRE(site.has_value());
    auto resolved = qoderz::config::resolve_site(**site, "");
    REQUIRE(resolved.has_value());
    auto overridden = qoderz::config::apply_loopback_override(*resolved, m_upstream.origin());
    REQUIRE(overridden.has_value());
    m_site = std::move(*overridden);

    auto record = make_account();
    credential_file file;
    file.m_accounts.emplace(record.key(), record);
    REQUIRE(m_store.save(file).has_value());

    std::vector<pool_account> accounts;
    accounts.push_back(pool_account{.m_key = record.key(), .m_account = record, .m_site = m_site});
    auto prepared = account_pool::prepare(accounts, pool_options{});
    REQUIRE(prepared.has_value());
    m_pool = std::make_unique<account_pool>(std::move(accounts), m_health, m_affinity,
                                            pool_options{}, std::move(*prepared));
    m_models.emplace(*m_pool, m_catalog, m_tokens, m_clock);

    json const app = json::array({json{{"key", "ultimate"},
                                       {"display_name", "Ultimate"},
                                       {"enable", true},
                                       {"max_input_tokens", 1000000}}});
    m_listing_body = json{{"app", app}, {"chat", json::array()}}.dump();

    server_config config;
    config.m_listen_host = "127.0.0.1";
    config.m_port = 0;
    config.m_api_key = std::move(key);
    config.m_max_body_bytes = max_body_bytes;
    m_server.emplace(m_io, std::move(config),
                     server_dependencies{.m_chat = m_chat, .m_models = *m_models});
    auto started = m_server->start();
    REQUIRE(started.has_value());
    m_port = *started;
    asio::co_spawn(m_io, m_server->run(), asio::detached);
  }

  ~server_harness() {
    if (m_server.has_value()) {
      m_server->stop();
    }
  }

  server_harness(server_harness const&) = delete;
  server_harness(server_harness&&) = delete;
  server_harness& operator=(server_harness const&) = delete;
  server_harness& operator=(server_harness&&) = delete;

  [[nodiscard]] static account_record make_account() {
    account_record record;
    record.m_site = std::string{site_id};
    record.m_name = "ada";
    record.m_access_token = "test-access-token-not-real";
    record.m_refresh_token = "test-refresh-token-not-real";
    record.m_expires_at = now_ms + one_hour_ms;
    record.m_uid = "uid-ada";
    record.m_display_name = "ada";
    record.m_machine_id = "machine-test-not-real";
    return record;
  }

  [[nodiscard]] std::optional<http_result>
  request(std::string method, std::string target,
          std::vector<std::pair<std::string, std::string>> headers = {}, std::string body = {},
          bool keep_alive = false) {
    auto wire = build_request(method, target, headers, body, keep_alive);
    auto nested = run_awaitable(m_io, one_request(m_port, std::move(wire)));
    if (!nested.has_value()) {
      return std::nullopt;
    }
    return std::move(*nested);
  }

  std::string m_listing_body;
};

inline std::pair<std::string, std::string> bearer(std::string_view key) {
  return {"Authorization", "Bearer " + std::string{key}};
}

} // namespace qoderz::test
