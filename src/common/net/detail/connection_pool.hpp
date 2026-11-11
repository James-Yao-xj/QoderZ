#pragma once

#include "common/net/detail/connection.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::net::detail {

/// Idle connections keyed by origin.
///
/// Only idle connections live here: `take` removes a connection before handing it out, so no two
/// coroutines ever hold the same connection and the connection itself needs no lock. The mutex
/// guards the idle lists and is never held across a `co_await`; a connection evicted by the cap or
/// by `close()` is closed after the lock is released.
class connection_pool {
public:
  connection_pool(std::size_t max_idle_per_origin, std::chrono::seconds idle_ttl);

  connection_pool(connection_pool const&) = delete;
  connection_pool& operator=(connection_pool const&) = delete;

  ~connection_pool();

  /// Removes and returns a live idle connection, dropping entries older than the TTL first.
  /// `nullptr` means nothing usable was idle and the caller must connect.
  [[nodiscard]] std::shared_ptr<connection> take(std::string const& origin);

  /// Returns a connection for reuse, or closes it when the origin already holds its idle cap.
  void give(std::string const& origin, std::shared_ptr<connection> idle_connection);

  /// Closes every idle connection and forgets every origin.
  void close();

  [[nodiscard]] std::size_t idle_count() const;

private:
  struct idle_entry {
    std::shared_ptr<connection> m_connection;
    std::chrono::steady_clock::time_point m_idle_since;
  };

  std::size_t m_max_idle_per_origin;
  std::chrono::seconds m_idle_ttl;
  mutable std::mutex m_mutex;
  std::vector<std::pair<std::string, std::vector<idle_entry>>> m_idle;
};

} // namespace qoderz::net::detail
