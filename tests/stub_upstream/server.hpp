#pragma once

#include "support/stub_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace qoderz::test {

/// Loopback HTTP/1.1 listener for `qoderz_stub_upstream`, with an explicit port because the
/// executable must be able to listen on the documented 18899. One coroutine per connection,
/// sequential requests per connection, keep-alive until either side closes, and `m_chunked`
/// responses written as one HTTP chunk per SSE frame.
class stub_upstream_server {
public:
  stub_upstream_server(boost::asio::io_context& io, std::uint16_t port, stub_handler handler);
  ~stub_upstream_server();

  stub_upstream_server(stub_upstream_server const&) = delete;
  stub_upstream_server& operator=(stub_upstream_server const&) = delete;

  [[nodiscard]] qoderz::result<std::uint16_t> start();

  [[nodiscard]] std::uint16_t port() const noexcept;

  [[nodiscard]] std::string origin() const;

  void stop();

  struct state;

private:
  std::shared_ptr<state> m_state;
};

} // namespace qoderz::test
