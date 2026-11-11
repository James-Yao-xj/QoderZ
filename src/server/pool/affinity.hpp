#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qoderz::server {

/// Which account a session was last served by, per `docs/design/server.md` §6.3.
///
/// The table maps a derived session id to an account key. A hit refreshes the binding's age and
/// moves it to the front of the LRU list; a binding that sat idle for `ttl_ms` is dropped, and a
/// full table evicts the least recently used binding. The table is process-local and never touches
/// the credential file.
///
/// Every member is safe to call from several threads; the mutex guards the map, the LRU list and
/// nothing else.
class session_affinity {
public:
  explicit session_affinity(std::size_t capacity = 512, std::int64_t ttl_ms = 2 * 60 * 60 * 1000);

  /// The account key bound to `session_id`, or `std::nullopt` when the session is unknown or its
  /// binding sat idle for longer than the TTL. A hit refreshes the binding.
  [[nodiscard]] std::optional<std::string> bound(std::string_view session_id, std::int64_t now_ms);

  /// Binds `session_id` to `account_key`, replacing an older binding. An empty session id is
  /// ignored: an anonymous request has nothing to stick to.
  void bind(std::string_view session_id, std::string account_key, std::int64_t now_ms);

  [[nodiscard]] std::size_t size() const;

private:
  struct binding {
    std::string m_account_key;
    std::int64_t m_last_used_ms = 0;
    std::list<std::string>::iterator m_lru;
  };

  void evict_expired_locked(std::int64_t now_ms);
  void evict_least_recent_locked();

  std::size_t m_capacity;
  std::int64_t m_ttl_ms;
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, binding> m_bindings;
  std::list<std::string> m_lru;
};

} // namespace qoderz::server
