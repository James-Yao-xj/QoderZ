#pragma once

#include "common/error.hpp"
#include "server/config.hpp"
#include "server/endpoints/models.hpp"
#include "server/http/chat_handler.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>

namespace qoderz::server {

/// The handlers `serve` injects. Both are owned by the caller and must outlive the server.
struct server_dependencies {
  chat_handler& m_chat;
  endpoints::models_handler& m_models;
};

/// The HTTP listener of docs/design/server.md §1, §2 and §7.
///
/// One coroutine per connection, requests served sequentially, no connection level lock: the order
/// of a request and of its SSE frames is the order of the coroutine's awaits. `start()` only binds
/// and listens, so a caller can learn the real port of a `:0` listener before serving.
class server {
public:
  server(boost::asio::io_context& io, server_config config, server_dependencies deps);
  ~server();

  server(server const&) = delete;
  server(server&&) = delete;
  server& operator=(server const&) = delete;
  server& operator=(server&&) = delete;

  /// Validates the safety rules of §1, then binds and listens. Returns the bound port, which is the
  /// configured one unless the configuration asked for 0.
  [[nodiscard]] result<std::uint16_t> start();

  /// Stops accepting new connections. The acceptor is closed on the io_context, so this is safe to
  /// call from a signal handler thread; in-flight requests keep their sockets.
  void stop();

  /// Waits for in-flight requests, at most `shutdown_grace`, then closes whatever is left.
  ///
  /// This is the grace period of §7: the caller stops the acceptor, drains, and exits 0. A test
  /// that does not want to wait simply never awaits it — the destructor closes the sockets instead.
  [[nodiscard]] boost::asio::awaitable<void> drain();

  /// The accept loop; returns when the acceptor is closed or fails.
  [[nodiscard]] boost::asio::awaitable<void> run();

  [[nodiscard]] std::uint16_t port() const noexcept;

  struct session;
  struct state;

private:
  std::shared_ptr<state> m_state;
};

} // namespace qoderz::server
