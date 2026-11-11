#pragma once

#include "common/net/http_types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qoderz::test {

struct stub_request {
  std::string m_method;
  std::string m_target;
  std::vector<qoderz::net::http_header> m_headers;
  std::string m_body;
};

struct stub_response {
  int m_status = 200;
  std::vector<qoderz::net::http_header> m_headers;
  std::string m_body;
  bool m_chunked = false;
  std::vector<std::string> m_chunks;
  std::chrono::milliseconds m_delay_before_headers{0};
  std::chrono::milliseconds m_delay_between_chunks{0};
  bool m_close_after = false;
  /// Writes the headers, then half of `m_body`, then drops the connection. Only for tests that need
  /// a peer to vanish in the middle of a response.
  bool m_truncate_body = false;
  /// Writes half of the header block, then drops the connection. Only for tests that need a peer to
  /// vanish after the client has already seen response bytes.
  bool m_truncate_header = false;
  /// Writes the whole response, without `Connection: close`, and then drops the connection. Only
  /// for tests that need the client to pool a connection the peer has already abandoned.
  bool m_close_silently = false;
  /// Answers with `HTTP/1.0`, which makes the connection single-use even without `Connection:
  /// close`.
  bool m_http10 = false;
};

using stub_handler = std::function<boost::asio::awaitable<stub_response>(stub_request const&)>;

/// A loopback HTTP/1.1 server for tests, bound to `127.0.0.1:0`.
///
/// One coroutine per accepted connection, sequential requests per connection, keep-alive until the
/// client or the handler says otherwise. `connection_count()` counts accepts, which is how a test
/// observes whether the client reused a connection. No name is ever resolved and no non-loopback
/// address is ever touched; the destructor stops the listener so a test cannot leak one.
class stub_server {
public:
  stub_server(boost::asio::io_context& io, stub_handler handler);
  ~stub_server();

  stub_server(stub_server const&) = delete;
  stub_server& operator=(stub_server const&) = delete;

  /// Binds and starts accepting. Fails with `error_kind::io` when the loopback listener cannot be
  /// set up; port 0 asks the kernel for a free port.
  [[nodiscard]] qoderz::result<std::uint16_t> start();

  [[nodiscard]] std::uint16_t port() const noexcept;

  /// `http://127.0.0.1:<port>`, ready to be concatenated with a target.
  [[nodiscard]] std::string origin() const;

  [[nodiscard]] std::size_t connection_count() const noexcept;

  /// Requests the handler has answered, including those whose response was truncated.
  [[nodiscard]] std::size_t request_count() const noexcept;

  void stop();

  struct state;

private:
  std::shared_ptr<state> m_state;
};

} // namespace qoderz::test
