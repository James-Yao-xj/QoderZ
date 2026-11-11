#include "stub_upstream/server.hpp"

#include "common/error.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

struct stub_upstream_server::state {
  asio::io_context& m_io;
  stub_handler m_handler;
  tcp::acceptor m_acceptor;
  std::uint16_t m_port = 0;
  std::atomic<std::size_t> m_connections{0};
  std::atomic<std::size_t> m_requests{0};
  bool m_stopped = false;

  state(asio::io_context& io, std::uint16_t port, stub_handler handler)
      : m_io{io}, m_handler{std::move(handler)}, m_acceptor{io}, m_port{port} {}
};

namespace {
std::unexpected<qoderz::error> listen_error(std::string_view detail) {
  return std::unexpected(qoderz::make_error(
      qoderz::error_kind::io, "stub upstream could not listen: " + std::string{detail}));
}

std::string hex_size(std::size_t size) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  do {
    text.push_back(digits[size % 16]);
    size /= 16;
  } while (size > 0);
  std::reverse(text.begin(), text.end());
  return text;
}

std::string head_of(stub_response const& response) {
  std::string wire;
  wire.append("HTTP/1.1 ");
  wire.append(std::to_string(response.m_status));
  wire.append(" ");
  wire.append(http::obsolete_reason(static_cast<http::status>(response.m_status)));
  wire.append("\r\n");
  for (auto const& header : response.m_headers) {
    wire.append(header.m_name);
    wire.append(": ");
    wire.append(header.m_value);
    wire.append("\r\n");
  }
  if (response.m_chunked) {
    wire.append("Transfer-Encoding: chunked\r\n");
  } else {
    std::size_t size = response.m_body.size();
    for (auto const& chunk : response.m_chunks) {
      size += chunk.size();
    }
    wire.append("Content-Length: ");
    wire.append(std::to_string(size));
    wire.append("\r\n");
  }
  if (response.m_close_after) {
    wire.append("Connection: close\r\n");
  }
  wire.append("\r\n");
  return wire;
}

/// Writes one response; the answer says whether the connection may serve another request.
boost::asio::awaitable<bool> write_response(beast::tcp_stream& stream,
                                            stub_response const& response) {
  auto const head = head_of(response);
  auto [head_ec, head_written] = co_await asio::async_write(stream.socket(), asio::buffer(head),
                                                            asio::as_tuple(asio::use_awaitable));
  (void)head_written;
  if (head_ec) {
    co_return false;
  }

  if (response.m_chunked) {
    for (auto const& chunk : response.m_chunks) {
      std::string frame;
      frame.append(hex_size(chunk.size()));
      frame.append("\r\n");
      frame.append(chunk);
      frame.append("\r\n");
      auto [ec, written] = co_await asio::async_write(stream.socket(), asio::buffer(frame),
                                                      asio::as_tuple(asio::use_awaitable));
      (void)written;
      if (ec) {
        co_return false;
      }
    }
    std::string const last{"0\r\n\r\n"};
    auto [last_ec, last_written] = co_await asio::async_write(stream.socket(), asio::buffer(last),
                                                              asio::as_tuple(asio::use_awaitable));
    (void)last_written;
    co_return !last_ec;
  }

  std::string body = response.m_body;
  for (auto const& chunk : response.m_chunks) {
    body.append(chunk);
  }
  if (body.empty()) {
    co_return true;
  }
  auto [body_ec, body_written] = co_await asio::async_write(stream.socket(), asio::buffer(body),
                                                            asio::as_tuple(asio::use_awaitable));
  (void)body_written;
  co_return !body_ec;
}

stub_request to_stub_request(http::request<http::string_body> const& message) {
  stub_request request;
  request.m_method = std::string{message.method_string()};
  request.m_target = std::string{message.target()};
  for (auto const& field : message.base()) {
    request.m_headers.push_back(qoderz::net::http_header{.m_name = std::string{field.name_string()},
                                                         .m_value = std::string{field.value()}});
  }
  request.m_body = message.body();
  return request;
}

boost::asio::awaitable<void> serve_connection(std::shared_ptr<stub_upstream_server::state> state,
                                              beast::tcp_stream stream) {
  beast::flat_buffer buffer;
  for (;;) {
    http::request_parser<http::string_body> parser;
    parser.body_limit(64 * 1024 * 1024);
    auto [read_ec, read_bytes] =
        co_await http::async_read(stream, buffer, parser, asio::as_tuple(asio::use_awaitable));
    (void)read_bytes;
    if (read_ec) {
      co_return;
    }

    auto const keep_alive = parser.get().keep_alive();
    auto const request = to_stub_request(parser.get());
    state->m_requests.fetch_add(1);
    auto response = co_await state->m_handler(request);
    bool const reusable = co_await write_response(stream, response);
    if (!reusable || !keep_alive || response.m_close_after || state->m_stopped) {
      co_return;
    }
  }
}

boost::asio::awaitable<void> accept_loop(std::shared_ptr<stub_upstream_server::state> state) {
  for (;;) {
    if (state->m_stopped) {
      co_return;
    }
    auto executor = co_await asio::this_coro::executor;
    auto [ec, socket] =
        co_await state->m_acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
      co_return;
    }
    state->m_connections.fetch_add(1);
    beast::tcp_stream stream{executor};
    stream.socket() = std::move(socket);
    asio::co_spawn(executor, serve_connection(state, std::move(stream)), asio::detached);
  }
}

} // namespace

stub_upstream_server::stub_upstream_server(asio::io_context& io, std::uint16_t port,
                                           stub_handler handler)
    : m_state{std::make_shared<state>(io, port, std::move(handler))} {}

stub_upstream_server::~stub_upstream_server() {
  stop();
}

qoderz::result<std::uint16_t> stub_upstream_server::start() {
  beast::error_code ec;
  tcp::endpoint const endpoint{asio::ip::address_v4::loopback(), m_state->m_port};
  m_state->m_acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    return listen_error(ec.message());
  }
  m_state->m_acceptor.set_option(asio::socket_base::reuse_address{true}, ec);
  if (ec) {
    return listen_error(ec.message());
  }
  m_state->m_acceptor.bind(endpoint, ec);
  if (ec) {
    return listen_error(ec.message());
  }
  m_state->m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return listen_error(ec.message());
  }
  m_state->m_port = m_state->m_acceptor.local_endpoint(ec).port();
  if (ec) {
    return listen_error(ec.message());
  }
  asio::co_spawn(m_state->m_io, accept_loop(m_state), asio::detached);
  return m_state->m_port;
}

std::uint16_t stub_upstream_server::port() const noexcept {
  return m_state->m_port;
}

std::string stub_upstream_server::origin() const {
  return "http://127.0.0.1:" + std::to_string(m_state->m_port);
}

void stub_upstream_server::stop() {
  if (!m_state || m_state->m_stopped) {
    return;
  }
  m_state->m_stopped = true;
  beast::error_code ignored;
  m_state->m_acceptor.close(ignored);
}

} // namespace qoderz::test
