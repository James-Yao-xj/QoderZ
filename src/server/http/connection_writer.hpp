#pragma once

#include "common/error.hpp"
#include "server/http/response_sink.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <string>
#include <string_view>

namespace qoderz::server {

/// Writes one complete response under the response timeout.
///
/// A peer that went away is `error_kind::canceled` — the caller stops and says nothing. Any other
/// transport failure is `error_kind::io`, which the error table renders as a 502 upstream error.
[[nodiscard]] boost::asio::awaitable<result<void>>
write_response(boost::beast::tcp_stream& stream,
               boost::beast::http::response<boost::beast::http::string_body> response);

/// A `string_body` response with `Content-Type: application/json` and the keep-alive decision.
[[nodiscard]] boost::beast::http::response<boost::beast::http::string_body>
json_response(unsigned version, bool keep_alive, int status, std::string body);

/// The `response_sink` of one connection.
///
/// SSE frames go through `http::make_chunk` rather than a chunked `buffer_body` serializer: a
/// `buffer_body` write is only ever partially consumed by one `async_write_some`, so the serializer
/// can stop mid-frame, while the chunk encoder always writes one whole frame and its CRLF.
class connection_writer final : public response_sink {
public:
  connection_writer(boost::beast::tcp_stream& stream, unsigned version, bool keep_alive);
  connection_writer(connection_writer const&) = delete;
  connection_writer(connection_writer&&) = delete;
  connection_writer& operator=(connection_writer const&) = delete;
  connection_writer& operator=(connection_writer&&) = delete;
  ~connection_writer() override = default;

  [[nodiscard]] bool started() const noexcept override;
  [[nodiscard]] bool reusable() const noexcept override;

  /// Whether frames may still be written, which is false again once `end_sse()` ran. Only this
  /// class needs that answer; a caller that wants to know about the connection asks `reusable()`.
  [[nodiscard]] bool streaming() const noexcept;
  boost::asio::awaitable<result<void>> write_json(int status, std::string body) override;
  boost::asio::awaitable<result<void>> begin_sse() override;
  boost::asio::awaitable<result<void>> write_sse(std::string_view frame) override;
  boost::asio::awaitable<result<void>> end_sse() override;

private:
  boost::beast::tcp_stream& m_stream;
  unsigned m_version;
  bool m_keep_alive;
  bool m_started = false;
  bool m_streaming = false;
  /// Set by `begin_sse()` and never cleared: the head already promised `Connection: close`, so the
  /// connection stays spent even after the last chunk went out.
  bool m_streamed = false;
};

} // namespace qoderz::server
