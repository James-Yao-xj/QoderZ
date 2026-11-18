#pragma once

#include "common/net/http_types.hpp"
#include "common/net/url.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace qoderz::net::detail {

/// One plain or TLS HTTP/1.1 connection, driven by the HTTP/1.1 framing rules only.
///
/// Holds no mutex and no shared state: exactly one coroutine may use a connection at a time, which
/// the pool guarantees by removing a connection from the idle list before handing it out. Deadlines
/// are passed per operation instead of stored, because a pooled connection must adopt the timeout
/// of whoever took it, not of the request that created it.
class connection {
public:
  /// Connects and, for a TLS URL, completes the handshake with SNI and host-name verification.
  [[nodiscard]] static boost::asio::awaitable<result<std::shared_ptr<connection>>>
  open(parsed_url const& url, std::chrono::milliseconds connect_timeout);

  connection(boost::asio::any_io_executor executor, parsed_url url, bool tls);
  connection(connection const&) = delete;
  connection& operator=(connection const&) = delete;
  ~connection();

  [[nodiscard]] bool is_tls() const noexcept;

  /// Writes one request, borrowing its body until completion. Keep `request` alive and unchanged
  /// while awaiting this call. A failure leaves the connection unusable, so the caller drops it.
  boost::asio::awaitable<result<void>> send(parsed_url const& url, http_request const& request,
                                            std::chrono::milliseconds timeout);

  /// Reads the status line and headers only, leaving the body in the stream.
  ///
  /// `max_body_bytes == 0` disables the framing limit, which is what streaming callers want; a
  /// buffered caller passes its cap so an oversized `Content-Length` fails before the body arrives.
  boost::asio::awaitable<result<http_response>> read_header(std::size_t max_body_bytes,
                                                            std::chrono::milliseconds timeout);

  /// Reads the next body chunk, or `nullopt` once the message is complete.
  ///
  /// A peer that closes before the message ends is an error, never a short read: silently
  /// truncating a body would let a partial upstream answer look successful.
  boost::asio::awaitable<result<std::optional<std::string>>>
  read_some(std::chrono::milliseconds timeout);

  /// Whether the completed message allows reuse. Meaningful only after `read_some` reported the end
  /// of the message.
  [[nodiscard]] bool keep_alive() const noexcept;

  /// Whether any byte of the current response has been parsed. Only a failure here being false is
  /// retryable, because then the server has not committed to an answer yet.
  [[nodiscard]] bool response_started() const noexcept;

  /// Closes the socket without further I/O. Safe from a destructor, after a failure, and from a
  /// watchdog that shares this connection.
  void close() noexcept;

private:
  struct impl;

  std::unique_ptr<impl> m_impl;
};

} // namespace qoderz::net::detail
