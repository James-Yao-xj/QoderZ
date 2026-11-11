#include "server/server.hpp"

#include "common/json.hpp"
#include "common/log.hpp"
#include "common/strings.hpp"
#include "server/api_key.hpp"
#include "server/endpoints/health.hpp"
#include "server/error_writer.hpp"
#include "server/http/connection_writer.hpp"
#include "server/router.hpp"

#include <atomic>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::server {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

constexpr std::string_view request_too_large_message =
    "the request body is larger than the server accepts";
constexpr std::string_view request_too_large_code = "request_too_large";
constexpr std::string_view unparsable_message = "the request could not be parsed";
constexpr std::string_view invalid_key_message = "invalid API key";

/// How often `drain` re-checks whether the last request finished. Shutdown only, so the coarse
/// interval costs nothing while the server is running.
constexpr std::chrono::milliseconds drain_poll_interval{20};

bool wants_stream(std::string_view body) {
  json const parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return false;
  }
  auto const flag = parsed.find("stream");
  return flag != parsed.end() && flag->is_boolean() && flag->get<bool>();
}

std::string session_id_of(http::request<http::string_body> const& request) {
  for (auto const name : {"X-Qoder2Api-Session", "X-Session-Id"}) {
    auto const found = request.find(name);
    if (found == request.end()) {
      continue;
    }
    auto const value = strings::trim(found->value());
    if (!value.empty()) {
      return std::string{value};
    }
  }
  return {};
}

std::string_view authorization_of(http::request<http::string_body> const& request) {
  auto const found = request.find(http::field::authorization);
  if (found == request.end()) {
    return {};
  }
  return found->value();
}

} // namespace

/// One accepted connection, plus the flag that tells `stop` whether a request is running on it.
struct server::session {
  explicit session(asio::io_context& io) : m_stream{io} {}

  beast::tcp_stream m_stream;
  std::atomic<bool> m_in_flight{false};
};

struct server::state {
  asio::io_context& m_io;
  server_config m_config;
  server_dependencies m_deps;
  tcp::acceptor m_acceptor;
  std::uint16_t m_port = 0;
  std::atomic<bool> m_stopping{false};
  std::mutex m_mutex;
  std::set<std::shared_ptr<session>> m_sessions;

  state(asio::io_context& io, server_config config, server_dependencies deps)
      : m_io{io}, m_config{std::move(config)}, m_deps{deps}, m_acceptor{io} {}
};

namespace {

using session_ptr = std::shared_ptr<server::session>;

void close_socket(server::session& self) {
  beast::error_code ignored;
  self.m_stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
  self.m_stream.socket().close(ignored);
}

void close_all_sessions(server::state& self) {
  std::set<session_ptr> sessions;
  {
    std::lock_guard const lock{self.m_mutex};
    sessions.swap(self.m_sessions);
  }
  for (auto const& one : sessions) {
    close_socket(*one);
  }
}

std::size_t session_count(server::state& self) {
  std::lock_guard const lock{self.m_mutex};
  return self.m_sessions.size();
}

/// Stops the accept loop and wakes the connections that are only waiting for their next request.
/// A connection with a request in flight keeps its socket: §7 gives it the grace period, and only
/// `drain` may cut it off.
void stop_accepting(server::state& self) {
  beast::error_code ignored;
  self.m_acceptor.close(ignored);

  std::vector<session_ptr> idle;
  {
    std::lock_guard const lock{self.m_mutex};
    for (auto const& one : self.m_sessions) {
      if (!one->m_in_flight.load()) {
        idle.push_back(one);
      }
    }
  }
  for (auto const& one : idle) {
    one->m_stream.socket().cancel(ignored);
  }
}

/// Keeps one session in the registry for as long as its coroutine runs, so `stop` and `drain` can
/// find it without touching the coroutine itself.
class session_slot {
public:
  session_slot(std::shared_ptr<server::state> self, session_ptr held)
      : m_self{std::move(self)}, m_held{std::move(held)} {}

  session_slot(session_slot const&) = delete;
  session_slot(session_slot&&) = delete;
  session_slot& operator=(session_slot const&) = delete;
  session_slot& operator=(session_slot&&) = delete;

  ~session_slot() {
    std::lock_guard const lock{m_self->m_mutex};
    m_self->m_sessions.erase(m_held);
  }

  [[nodiscard]] beast::tcp_stream& stream() noexcept { return m_held->m_stream; }

  void begin_request() noexcept { m_held->m_in_flight.store(true); }

  void end_request() noexcept { m_held->m_in_flight.store(false); }

private:
  std::shared_ptr<server::state> m_self;
  session_ptr m_held;
};

asio::awaitable<bool> send_error(beast::tcp_stream& stream, unsigned version, bool keep_alive,
                                 error const& failure) {
  if (failure.m_kind == error_kind::canceled) {
    co_return false;
  }
  auto response = json_response(version, keep_alive, http_status_for(failure), to_json(failure));
  if (auto const retry = retry_after_for(failure); retry.has_value()) {
    response.set(http::field::retry_after, std::to_string(*retry));
  }
  auto sent = co_await write_response(stream, std::move(response));
  co_return sent.has_value() && keep_alive;
}

asio::awaitable<bool> send_json(beast::tcp_stream& stream, unsigned version, bool keep_alive,
                                int status, std::string body) {
  auto sent =
      co_await write_response(stream, json_response(version, keep_alive, status, std::move(body)));
  co_return sent.has_value() && keep_alive;
}

asio::awaitable<bool> handle_chat(server::state& self,
                                  http::request<http::string_body> const& request,
                                  beast::tcp_stream& stream, bool keep_alive) {
  connection_writer writer{stream, request.version(), keep_alive};
  chat_http_request chat;
  chat.m_body = request.body();
  chat.m_stream = wants_stream(chat.m_body);
  chat.m_session_id = session_id_of(request);

  auto outcome = co_await self.m_deps.m_chat.handle(chat, writer);
  if (outcome) {
    co_return keep_alive && !writer.streaming();
  }

  auto failure = std::move(outcome.error());
  if (!writer.started()) {
    auto const sent = co_await send_error(stream, request.version(), keep_alive, failure);
    co_return sent;
  }
  if (writer.streaming()) {
    auto const frame = to_sse_frame(failure);
    (void)co_await writer.write_sse(frame);
    (void)co_await writer.end_sse();
  }
  co_return false;
}

/// One request, from the route table to the answer. The returned flag says whether the connection
/// may serve another request; every path that could leave unread bytes on the wire says no.
asio::awaitable<bool> handle_request(server::state& self,
                                     http::request<http::string_body> const& request,
                                     beast::tcp_stream& stream, bool keep_alive) {
  auto const match = match_route(request.method_string(), request.target());
  log(log_level::debug, "request",
      {{"method", request.method_string()}, {"path", path_of(request.target())}});

  if (match.m_kind == route_kind::not_found) {
    co_return co_await send_error(stream, request.version(), keep_alive, not_found_error());
  }
  if (match.m_kind == route_kind::health) {
    co_return co_await send_json(stream, request.version(), keep_alive, 200,
                                 endpoints::health_json());
  }

  if (requires_authentication(request.target()) &&
      !authorize(self.m_config.m_api_key, authorization_of(request))) {
    co_return co_await send_error(stream, request.version(), keep_alive,
                                  auth_error(std::string{invalid_key_message}));
  }
  if (match.m_kind == route_kind::wrong_method) {
    auto const failure = wrong_method_error(match.m_allow);
    auto response =
        json_response(request.version(), keep_alive, http_status_for(failure), to_json(failure));
    response.set(http::field::allow, match.m_allow);
    auto const sent = co_await write_response(stream, std::move(response));
    co_return sent.has_value() && keep_alive;
  }
  if (match.m_kind == route_kind::models) {
    auto built = co_await self.m_deps.m_models.build_json();
    if (!built) {
      co_return co_await send_error(stream, request.version(), keep_alive,
                                    std::move(built.error()));
    }
    co_return co_await send_json(stream, request.version(), keep_alive, 200, std::move(*built));
  }
  co_return co_await handle_chat(self, request, stream, keep_alive);
}

asio::awaitable<void> serve_connection(std::shared_ptr<server::state> self, session_ptr held) {
  session_slot slot{self, std::move(held)};
  auto& stream = slot.stream();
  beast::flat_buffer buffer;

  for (;;) {
    if (self->m_stopping.load()) {
      co_return;
    }
    http::request_parser<http::string_body> parser;
    parser.body_limit(self->m_config.m_max_body_bytes);
    parser.header_limit(max_header_bytes);
    stream.expires_after(request_timeout);
    auto [ec, read_bytes] =
        co_await http::async_read(stream, buffer, parser, asio::as_tuple(asio::use_awaitable));
    (void)read_bytes;
    if (ec) {
      if (ec == http::error::end_of_stream || ec == asio::error::eof ||
          ec == asio::error::operation_aborted) {
        co_return;
      }
      if (ec == http::error::body_limit) {
        (void)co_await send_error(stream, parser.get().version(), false,
                                  client_status_error(413, std::string{request_too_large_message},
                                                      std::string{request_too_large_code}));
        co_return;
      }
      (void)co_await send_error(stream, parser.get().version(), false,
                                client_status_error(400, std::string{unparsable_message}));
      co_return;
    }

    auto const& request = parser.get();
    bool const keep_alive = request.keep_alive();
    slot.begin_request();
    bool const reusable = co_await handle_request(*self, request, stream, keep_alive);
    slot.end_request();
    if (!reusable || self->m_stopping.load()) {
      co_return;
    }
  }
}

} // namespace

server::server(asio::io_context& io, server_config config, server_dependencies deps)
    : m_state{std::make_shared<state>(io, std::move(config), deps)} {}

server::~server() {
  m_state->m_stopping.store(true);
  close_all_sessions(*m_state);
}

result<std::uint16_t> server::start() {
  if (auto valid = validate_config(m_state->m_config); !valid) {
    return std::unexpected(std::move(valid.error()));
  }

  beast::error_code ec;
  tcp::endpoint endpoint;
  auto const address = asio::ip::make_address(m_state->m_config.m_listen_host, ec);
  if (!ec) {
    endpoint = tcp::endpoint{address, m_state->m_config.m_port};
  } else {
    tcp::resolver resolver{m_state->m_io};
    auto const results = resolver.resolve(m_state->m_config.m_listen_host,
                                          std::to_string(m_state->m_config.m_port), ec);
    if (ec || results.empty()) {
      return std::unexpected(configuration_error(
          "the listen host '" + m_state->m_config.m_listen_host + "' does not resolve"));
    }
    endpoint = results.begin()->endpoint();
  }

  m_state->m_acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    return std::unexpected(
        make_error(error_kind::io, "the listener could not open: " + ec.message()));
  }
  m_state->m_acceptor.set_option(asio::socket_base::reuse_address{true}, ec);
  if (ec) {
    return std::unexpected(
        make_error(error_kind::io, "the listener could not be configured: " + ec.message()));
  }
  m_state->m_acceptor.bind(endpoint, ec);
  if (ec) {
    return std::unexpected(
        make_error(error_kind::io, "the listener could not bind: " + ec.message()));
  }
  m_state->m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return std::unexpected(
        make_error(error_kind::io, "the listener could not listen: " + ec.message()));
  }
  m_state->m_port = m_state->m_acceptor.local_endpoint(ec).port();
  if (ec) {
    return std::unexpected(
        make_error(error_kind::io, "the listener port is unknown: " + ec.message()));
  }

  log(log_level::info, "listening",
      {{"host", m_state->m_config.m_listen_host},
       {"port", std::to_string(m_state->m_port)},
       {"site", m_state->m_config.m_site_id},
       {"auth", m_state->m_config.m_api_key.has_value() ? "api-key" : "open"}});
  return m_state->m_port;
}

void server::stop() {
  m_state->m_stopping.store(true);
  auto self = m_state;
  asio::post(self->m_io, [self] { stop_accepting(*self); });
}

boost::asio::awaitable<void> server::drain() {
  auto self = m_state;
  auto const deadline = std::chrono::steady_clock::now() + shutdown_grace;
  while (session_count(*self) != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      log(log_level::warn, "closing the remaining connections after the shutdown grace",
          {{"connections", std::to_string(session_count(*self))}});
      break;
    }
    asio::steady_timer timer{co_await asio::this_coro::executor, drain_poll_interval};
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
  }
  close_all_sessions(*self);
}

boost::asio::awaitable<void> server::run() {
  auto self = m_state;
  for (;;) {
    if (self->m_stopping.load()) {
      co_return;
    }
    auto [ec, socket] = co_await self->m_acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
      if (ec != asio::error::operation_aborted && !self->m_stopping.load()) {
        log(log_level::error, "accepting a connection failed", {{"error", ec.message()}});
      }
      co_return;
    }
    auto held = std::make_shared<session>(self->m_io);
    held->m_stream.socket() = std::move(socket);
    {
      std::lock_guard const lock{self->m_mutex};
      self->m_sessions.insert(held);
    }
    asio::co_spawn(self->m_io, serve_connection(self, std::move(held)), asio::detached);
  }
}

std::uint16_t server::port() const noexcept {
  return m_state->m_port;
}

} // namespace qoderz::server
