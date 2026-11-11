#include "common/net/detail/connection_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::net::detail {
namespace {

using idle_buckets = std::vector<std::pair<std::string, std::vector<std::shared_ptr<connection>>>>;

} // namespace

connection_pool::connection_pool(std::size_t max_idle_per_origin, std::chrono::seconds idle_ttl)
    : m_max_idle_per_origin{max_idle_per_origin}, m_idle_ttl{idle_ttl} {}

connection_pool::~connection_pool() {
  close();
}

std::shared_ptr<connection> connection_pool::take(std::string const& origin) {
  std::vector<std::shared_ptr<connection>> expired;
  std::shared_ptr<connection> found;
  {
    std::lock_guard const guard{m_mutex};
    auto const bucket = std::find_if(m_idle.begin(), m_idle.end(),
                                     [&](auto const& entry) { return entry.first == origin; });
    if (bucket == m_idle.end()) {
      return nullptr;
    }
    auto& entries = bucket->second;
    auto const now = std::chrono::steady_clock::now();
    for (auto it = entries.begin(); it != entries.end();) {
      if (now - it->m_idle_since < m_idle_ttl) {
        ++it;
        continue;
      }
      expired.push_back(std::move(it->m_connection));
      it = entries.erase(it);
    }
    if (!entries.empty()) {
      found = std::move(entries.back().m_connection);
      entries.pop_back();
    }
  }
  for (auto& stale : expired) {
    stale->close();
  }
  return found;
}

void connection_pool::give(std::string const& origin, std::shared_ptr<connection> idle_connection) {
  if (!idle_connection) {
    return;
  }
  std::shared_ptr<connection> evicted;
  {
    std::lock_guard const guard{m_mutex};
    auto bucket = std::find_if(m_idle.begin(), m_idle.end(),
                               [&](auto const& entry) { return entry.first == origin; });
    if (bucket == m_idle.end()) {
      bucket = m_idle.emplace(m_idle.end(), origin, std::vector<idle_entry>{});
    }
    auto& entries = bucket->second;
    if (entries.size() >= m_max_idle_per_origin) {
      evicted = std::move(idle_connection);
    } else {
      entries.push_back(idle_entry{.m_connection = std::move(idle_connection),
                                   .m_idle_since = std::chrono::steady_clock::now()});
    }
  }
  if (evicted) {
    evicted->close();
  }
}

void connection_pool::close() {
  decltype(m_idle) dropped;
  {
    std::lock_guard const guard{m_mutex};
    dropped.swap(m_idle);
  }
  for (auto& bucket : dropped) {
    for (auto& entry : bucket.second) {
      entry.m_connection->close();
    }
  }
}

std::size_t connection_pool::idle_count() const {
  std::lock_guard const guard{m_mutex};
  std::size_t total = 0;
  for (auto const& bucket : m_idle) {
    total += bucket.second.size();
  }
  return total;
}

} // namespace qoderz::net::detail
