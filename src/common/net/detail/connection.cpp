#include "common/net/detail/connection.hpp"

#include "common/net/detail/tls.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <openssl/ssl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::net::detail {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

using plain_stream = beast::tcp_stream;
using tls_stream = asio::ssl::stream<beast::tcp_stream>;

constexpr std::size_t chunk_bytes = 64 * 1024;

std::unexpected<error> stage_error(std::string_view host, std::string_view stage,
                                   std::string_view detail) {
  return std::unexpected(upstream_error("The HTTP request to " + std::string{host} + " failed at " +
                                        std::string{stage} + ": " + std::string{detail}));
}

std::unexpected<error> transfer_error(beast::error_code const& ec, std::string_view host,
                                      std::string_view stage) {
  return stage_error(host, stage, ec.message());
}

std::vector<http_header> copy_fields(http::fields const& fields) {
  std::vector<http_header> headers;
  for (auto const& field : fields) {
    headers.push_back(http_header{.m_name = std::string{field.name_string()},
                                  .m_value = std::string{field.value()}});
  }
  return headers;
}

} // namespace

/// The parser and its flat buffer survive across requests on a reused connection, which is what
/// lets bytes pipelined behind one response be consumed by the next one instead of being dropped.
struct connection::impl {
  asio::any_io_executor m_executor;
  parsed_url m_url;
  bool m_tls = false;
  plain_stream m_plain;
  std::unique_ptr<tls_stream> m_tls_stream;
  std::optional<http::response_parser<http::buffer_body>> m_parser;
  beast::flat_buffer m_buffer;
  std::string m_chunk;
  bool m_keep_alive = false;
  bool m_response_started = false;

  impl(asio::any_io_executor executor, parsed_url url, bool tls)
      : m_executor{std::move(executor)}, m_url{std::move(url)}, m_tls{tls}, m_plain{m_executor} {
    if (m_tls) {
      m_tls_stream = std::make_unique<tls_stream>(m_executor, tls_context::instance());
    }
  }

  [[nodiscard]] beast::tcp_stream& lowest() noexcept {
    return m_tls ? m_tls_stream->next_layer() : m_plain;
  }

  template <class function_type> [[nodiscard]] decltype(auto) visit(function_type&& function) {
    if (m_tls) {
      return function(*m_tls_stream);
    }
    return function(m_plain);
  }

  void close() noexcept {
    beast::error_code ignored;
    auto& socket = lowest().socket();
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
  }
};

connection::connection(asio::any_io_executor executor, parsed_url url, bool tls)
    : m_impl{std::make_unique<impl>(std::move(executor), std::move(url), tls)} {}

connection::~connection() {
  close();
}

bool connection::is_tls() const noexcept {
  return m_impl->m_tls;
}

void connection::close() noexcept {
  if (m_impl) {
    m_impl->close();
  }
}

bool connection::keep_alive() const noexcept {
  return m_impl->m_keep_alive;
}

bool connection::response_started() const noexcept {
  return m_impl->m_response_started;
}

namespace {

template <class stream_type>
boost::asio::awaitable<result<void>> resolve_and_connect(stream_type& stream, parsed_url const& url,
                                                         std::chrono::milliseconds timeout) {
  tcp::resolver resolver{stream.get_executor()};
  auto [resolve_ec, endpoints] =
      co_await resolver.async_resolve(url.m_host, url.m_port, asio::as_tuple(asio::use_awaitable));
  if (resolve_ec) {
    co_return stage_error(url.m_host, "connect", resolve_ec.message());
  }
  stream.expires_after(timeout);
  auto [connect_ec, endpoint] =
      co_await stream.async_connect(endpoints, asio::as_tuple(asio::use_awaitable));
  (void)endpoint;
  if (connect_ec) {
    co_return stage_error(url.m_host, "connect", connect_ec.message());
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> handshake(tls_stream& stream, parsed_url const& url,
                                               std::chrono::milliseconds timeout) {
  if (::SSL_set_tlsext_host_name(stream.native_handle(), url.m_host.c_str()) != 1) {
    co_return stage_error(url.m_host, "tls", "the handshake could not name the server");
  }
  stream.set_verify_callback(asio::ssl::host_name_verification(url.m_host));
  stream.next_layer().expires_after(timeout);
  auto [ec] = co_await stream.async_handshake(asio::ssl::stream_base::client,
                                              asio::as_tuple(asio::use_awaitable));
  if (ec) {
    co_return stage_error(url.m_host, "tls", ec.message());
  }
  co_return result<void>{};
}

} // namespace

boost::asio::awaitable<result<std::shared_ptr<connection>>>
connection::open(parsed_url const& url, std::chrono::milliseconds connect_timeout) {
  auto const executor = co_await asio::this_coro::executor;
  auto handle = std::make_shared<connection>(executor, url, url.m_tls);
  if (url.m_tls) {
    if (auto connected =
            co_await resolve_and_connect(handle->m_impl->lowest(), url, connect_timeout);
        !connected) {
      co_return std::unexpected(connected.error());
    }
    if (auto secured = co_await handshake(*handle->m_impl->m_tls_stream, url, connect_timeout);
        !secured) {
      co_return std::unexpected(secured.error());
    }
  } else if (auto connected =
                 co_await resolve_and_connect(handle->m_impl->m_plain, url, connect_timeout);
             !connected) {
    co_return std::unexpected(connected.error());
  }
  co_return handle;
}

boost::asio::awaitable<result<void>> connection::send(parsed_url const& url,
                                                      http_request const& request,
                                                      std::chrono::milliseconds timeout) {
  http::request<http::string_body> message;
  message.method_string(request.m_method);
  message.target(url.m_target);
  message.version(11);
  message.body() = request.m_body;
  for (auto const& header : request.m_headers) {
    message.set(header.m_name, header.m_value);
  }
  message.prepare_payload();

  auto& self = *m_impl;
  self.lowest().expires_after(timeout);
  auto [ec, written] = co_await self.visit([&](auto& stream) {
    return http::async_write(stream, message, asio::as_tuple(asio::use_awaitable));
  });
  (void)written;
  if (ec) {
    co_return transfer_error(ec, self.m_url.m_host, "send");
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<http_response>>
connection::read_header(std::size_t max_body_bytes, std::chrono::milliseconds timeout) {
  auto& self = *m_impl;
  self.m_response_started = false;
  self.m_parser.emplace();
  // Without eager parsing Beast stops on every chunk-size line and reports a successful read with
  // no body bytes, which would make a chunked response look empty until the terminating chunk.
  self.m_parser->eager(true);
  self.m_parser->body_limit(max_body_bytes == 0 ? boost::optional<std::uint64_t>{}
                                                : boost::optional<std::uint64_t>{max_body_bytes});
  self.lowest().expires_after(timeout);
  auto [ec, header_bytes] = co_await self.visit([&](auto& stream) {
    return http::async_read_header(stream, self.m_buffer, *self.m_parser,
                                   asio::as_tuple(asio::use_awaitable));
  });
  (void)header_bytes;
  if (ec) {
    if (ec == http::error::body_limit) {
      co_return std::unexpected(
          stage_error(self.m_url.m_host, "read", "the response body is larger than allowed"));
    }
    co_return transfer_error(ec, self.m_url.m_host, "read");
  }

  self.m_response_started = true;
  auto const& header = self.m_parser->get();
  http_response response;
  response.m_status = header.result_int();
  response.m_headers = copy_fields(header.base());
  self.m_keep_alive = header.keep_alive();
  co_return response;
}

boost::asio::awaitable<result<std::optional<std::string>>>
connection::read_some(std::chrono::milliseconds timeout) {
  auto& self = *m_impl;
  if (!self.m_parser || self.m_parser->is_done()) {
    co_return std::optional<std::string>{};
  }

  // A single Beast read returns nothing observable when it stops on chunk framing, so keep reading
  // until real body bytes show up or the message ends. Each attempt gets its own deadline, which is
  // what makes the idle timeout mean "silence while a body is owed".
  for (;;) {
    self.m_chunk.assign(chunk_bytes, '\0');
    auto& body = self.m_parser->get().body();
    body.data = self.m_chunk.data();
    body.size = self.m_chunk.size();
    body.more = true;

    self.lowest().expires_after(timeout);
    auto [ec, parsed_bytes] = co_await self.visit([&](auto& stream) {
      return http::async_read_some(stream, self.m_buffer, *self.m_parser,
                                   asio::as_tuple(asio::use_awaitable));
    });
    (void)parsed_bytes;

    auto const produced = self.m_chunk.size() - body.size;
    body.data = nullptr;
    body.size = 0;
    body.more = true;

    if (ec && ec != http::error::need_buffer) {
      if (ec == http::error::body_limit) {
        co_return std::unexpected(
            stage_error(self.m_url.m_host, "read", "the response body is larger than allowed"));
      }
      co_return transfer_error(ec, self.m_url.m_host, "read");
    }
    if (produced != 0) {
      co_return std::optional<std::string>{self.m_chunk.substr(0, produced)};
    }
    if (self.m_parser->is_done()) {
      co_return std::optional<std::string>{};
    }
  }
}

} // namespace qoderz::net::detail