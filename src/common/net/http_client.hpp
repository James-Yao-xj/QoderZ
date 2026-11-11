#pragma once

#include "common/error.hpp"
#include "common/net/http_types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <memory>

namespace qoderz::net {

struct http_client_options {
  std::chrono::milliseconds m_connect_timeout{15000};
  std::chrono::milliseconds m_request_timeout{60000};
  std::chrono::milliseconds m_stream_idle_timeout{300000};
  std::size_t m_max_buffered_body = 16 * 1024 * 1024;
  std::size_t m_max_idle_per_origin = 4;
  std::chrono::seconds m_idle_ttl{30};
};

/// HTTP/1.1 client with a per-origin idle pool.
///
/// Both entry points are safe to call from any number of coroutines on the client's `io_context`:
/// the pool is serialised by its own mutex and never held across an await, and each connection is
/// owned by exactly one request at a time. The client must outlive its coroutines; `send` and
/// `open` keep a reference to it so a client destroyed mid-flight refuses new work instead of
/// touching freed memory. `close()` only empties the idle pool — connections already handed to a
/// stream stay alive under that stream.
class http_client {
public:
  explicit http_client(boost::asio::io_context& io, http_client_options options = {});
  ~http_client();

  http_client(http_client const&) = delete;
  http_client& operator=(http_client const&) = delete;

  /// Sends a request and buffers the whole response body.
  ///
  /// 4xx and 5xx statuses are successful responses — mapping them to `error_kind` belongs to the
  /// caller. Transport, TLS, parse and timeout failures return `error_kind::upstream` with the
  /// host and stage in the message and never the headers or body; a body larger than
  /// `m_max_buffered_body` is reported the same way.
  [[nodiscard]] boost::asio::awaitable<result<http_response>> send(http_request const& request);

  /// Sends a request and returns a stream over the response body once the headers are parsed.
  ///
  /// Destroying the stream before its body ends closes that connection instead of pooling it; a
  /// fully read keep-alive response is returned to the pool.
  [[nodiscard]] boost::asio::awaitable<result<std::unique_ptr<http_stream>>>
  open(http_request const& request);

  /// Drops every idle connection.
  void close();

private:
  struct impl;

  std::shared_ptr<impl> m_impl;
};

} // namespace qoderz::net
