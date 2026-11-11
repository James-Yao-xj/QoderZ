#include "server/pool/affinity.hpp"

#include <utility>

namespace qoderz::server {

session_affinity::session_affinity(std::size_t capacity, std::int64_t ttl_ms)
    : m_capacity{capacity == 0 ? 1 : capacity}, m_ttl_ms{ttl_ms} {}

std::optional<std::string> session_affinity::bound(std::string_view session_id,
                                                   std::int64_t now_ms) {
  if (session_id.empty()) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> guard{m_mutex};
  evict_expired_locked(now_ms);

  auto const found = m_bindings.find(std::string{session_id});
  if (found == m_bindings.end()) {
    return std::nullopt;
  }

  found->second.m_last_used_ms = now_ms;
  m_lru.splice(m_lru.begin(), m_lru, found->second.m_lru);
  return found->second.m_account_key;
}

void session_affinity::bind(std::string_view session_id, std::string account_key,
                            std::int64_t now_ms) {
  if (session_id.empty()) {
    return;
  }

  std::lock_guard<std::mutex> guard{m_mutex};
  evict_expired_locked(now_ms);

  auto const found = m_bindings.find(std::string{session_id});
  if (found != m_bindings.end()) {
    found->second.m_account_key = std::move(account_key);
    found->second.m_last_used_ms = now_ms;
    m_lru.splice(m_lru.begin(), m_lru, found->second.m_lru);
    return;
  }

  while (m_bindings.size() >= m_capacity) {
    evict_least_recent_locked();
  }

  m_lru.push_front(std::string{session_id});
  binding fresh{};
  fresh.m_account_key = std::move(account_key);
  fresh.m_last_used_ms = now_ms;
  fresh.m_lru = m_lru.begin();
  m_bindings.emplace(m_lru.front(), std::move(fresh));
}

std::size_t session_affinity::size() const {
  std::lock_guard<std::mutex> guard{m_mutex};
  return m_bindings.size();
}

void session_affinity::evict_expired_locked(std::int64_t now_ms) {
  for (auto slot = m_bindings.begin(); slot != m_bindings.end();) {
    if (now_ms - slot->second.m_last_used_ms > m_ttl_ms) {
      m_lru.erase(slot->second.m_lru);
      slot = m_bindings.erase(slot);
    } else {
      ++slot;
    }
  }
}

void session_affinity::evict_least_recent_locked() {
  if (m_lru.empty()) {
    return;
  }
  m_bindings.erase(m_lru.back());
  m_lru.pop_back();
}

} // namespace qoderz::server
