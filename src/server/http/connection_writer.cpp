#include "server/http/connection_writer.hpp"

#include "server/config.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/write.hpp>
#include <utility>

namespace qoderz::server {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;

error socket_error(beast::error_code const& ec) {
  if (ec == asio::error::eof || ec == asio::error::connection_reset ||
      ec == asio::error::broken_pipe || ec == asio::error::operation_aborted ||
      ec == asio::error::not_connected) {
    return make_error(error_kind::canceled, "the client closed the connection");
  }
  return make_error(error_kind::io, "writing the response failed: " + ec.message());
}

} // namespace

boost::asio::awaitable<result<void>> write_response(beast::tcp_stream& stream,
                                                    http::response<http::string_body> response) {
  stream.expires_after(response_timeout);
  auto [ec, written] =
      co_await http::async_write(stream, response, asio::as_tuple(asio::use_awaitable));
  (void)written;
  if (ec) {
    co_return std::unexpected(socket_error(ec));
  }
  co_return result<void>{};
}

http::response<http::string_body> json_response(unsigned version, bool keep_alive, int status,
                                                std::string body) {
  http::response<http::string_body> response{static_cast<http::status>(status), version};
  response.set(http::field::content_type, "application/json");
  response.keep_alive(keep_alive);
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

connection_writer::connection_writer(beast::tcp_stream& stream, unsigned version, bool keep_alive)
    : m_stream{stream}, m_version{version}, m_keep_alive{keep_alive} {}

bool connection_writer::started() const noexcept {
  return m_started;
}

bool connection_writer::streaming() const noexcept {
  return m_streaming;
}

boost::asio::awaitable<result<void>> connection_writer::write_json(int status, std::string body) {
  if (m_started) {
    co_return std::unexpected(internal_error("the response has already started"));
  }
  auto written = co_await write_response(
      m_stream, json_response(m_version, m_keep_alive, status, std::move(body)));
  if (!written) {
    co_return std::unexpected(std::move(written.error()));
  }
  m_started = true;
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> connection_writer::begin_sse() {
  if (m_started) {
    co_return std::unexpected(internal_error("the response has already started"));
  }
  http::response<http::empty_body> head{http::status::ok, m_version};
  head.set(http::field::content_type, "text/event-stream");
  head.set(http::field::cache_control, "no-cache");
  head.set("X-Accel-Buffering", "no");
  head.chunked(true);
  // A stream is self-delimiting through its last chunk, but the connection is closed afterwards
  // anyway: a client that stopped reading mid-stream must not leave a half-consumed socket behind.
  head.keep_alive(false);

  http::response_serializer<http::empty_body> serializer{head};
  m_stream.expires_after(response_timeout);
  auto [ec, written] =
      co_await http::async_write_header(m_stream, serializer, asio::as_tuple(asio::use_awaitable));
  (void)written;
  if (ec) {
    co_return std::unexpected(socket_error(ec));
  }
  m_started = true;
  m_streaming = true;
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> connection_writer::write_sse(std::string_view frame) {
  if (!m_streaming) {
    co_return std::unexpected(internal_error("the stream has not started"));
  }
  auto const chunk = http::make_chunk(asio::buffer(frame.data(), frame.size()));
  m_stream.expires_after(response_timeout);
  auto [ec, written] =
      co_await asio::async_write(m_stream.socket(), chunk, asio::as_tuple(asio::use_awaitable));
  (void)written;
  if (ec) {
    co_return std::unexpected(socket_error(ec));
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> connection_writer::end_sse() {
  if (!m_streaming) {
    co_return std::unexpected(internal_error("the stream has not started"));
  }
  auto const last = http::make_chunk_last();
  m_stream.expires_after(response_timeout);
  auto [ec, written] =
      co_await asio::async_write(m_stream.socket(), last, asio::as_tuple(asio::use_awaitable));
  (void)written;
  m_streaming = false;
  if (ec) {
    co_return std::unexpected(socket_error(ec));
  }
  co_return result<void>{};
}

} // namespace qoderz::server
