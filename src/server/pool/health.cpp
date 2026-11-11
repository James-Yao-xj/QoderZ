#include "server/pool/health.hpp"

#include <utility>

namespace qoderz::server {
namespace {

// Locked at compile time so a later tuning pass cannot silently reshape the documented curve.
static_assert(account_health::cooldown_ms(failure_kind::quota, 1) == 5 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::quota, 2) == 10 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::quota, 4) == 40 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::quota, 5) == 60 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::auth, 1) == 15 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::auth, 2) == 30 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::auth, 3) == 60 * 60 * 1000);
static_assert(account_health::cooldown_ms(failure_kind::auth, 1000) == 60 * 60 * 1000);

} // namespace

account_health::account_health(clock& time) : m_clock{time} {}

void account_health::report_failure(std::string_view account_key, failure_kind kind) {
  auto const now = m_clock.now_ms();
  auto const key = std::string{account_key};

  std::lock_guard<std::mutex> guard{m_mutex};
  prune_locked(now);

  if (auto found = m_entries.find(key); found != m_entries.end()) {
    auto& recorded = found->second.m_state;
    if (recorded.m_kind == kind) {
      ++recorded.m_failures;
    } else {
      recorded.m_kind = kind;
      recorded.m_failures = 1;
    }
    recorded.m_cooldown_until_ms = now + cooldown_ms(recorded.m_kind, recorded.m_failures);
    found->second.m_prune_after_ms = recorded.m_cooldown_until_ms + entry_retention_ms;
    return;
  }

  entry fresh{};
  fresh.m_state.m_kind = kind;
  fresh.m_state.m_failures = 1;
  fresh.m_state.m_cooldown_until_ms = now + cooldown_ms(kind, 1);
  fresh.m_prune_after_ms = fresh.m_state.m_cooldown_until_ms + entry_retention_ms;
  m_entries.emplace(std::move(key), std::move(fresh));
}

void account_health::report_success(std::string_view account_key) {
  auto const now = m_clock.now_ms();
  auto const key = std::string{account_key};

  std::lock_guard<std::mutex> guard{m_mutex};
  prune_locked(now);
  m_entries.erase(key);
}

bool account_health::is_cooling(std::string_view account_key) const {
  auto const now = m_clock.now_ms();
  std::lock_guard<std::mutex> guard{m_mutex};
  auto const found = m_entries.find(std::string{account_key});
  return found != m_entries.end() && found->second.m_state.m_cooldown_until_ms > now;
}

std::optional<health_state> account_health::state(std::string_view account_key) const {
  std::lock_guard<std::mutex> guard{m_mutex};
  auto const found = m_entries.find(std::string{account_key});
  if (found == m_entries.end()) {
    return std::nullopt;
  }
  return found->second.m_state;
}

std::size_t account_health::cooling_count() const {
  auto const now = m_clock.now_ms();
  std::lock_guard<std::mutex> guard{m_mutex};
  std::size_t count = 0;
  for (auto const& slot : m_entries) {
    if (slot.second.m_state.m_cooldown_until_ms > now) {
      ++count;
    }
  }
  return count;
}

bool account_health::all_cooling_auth() const {
  auto const now = m_clock.now_ms();
  std::lock_guard<std::mutex> guard{m_mutex};

  bool saw_cooling = false;
  for (auto const& slot : m_entries) {
    if (slot.second.m_state.m_cooldown_until_ms <= now) {
      continue;
    }
    if (slot.second.m_state.m_kind != failure_kind::auth) {
      return false;
    }
    saw_cooling = true;
  }
  return saw_cooling;
}

void account_health::prune() {
  auto const now = m_clock.now_ms();
  std::lock_guard<std::mutex> guard{m_mutex};
  prune_locked(now);
}

std::int64_t account_health::now_ms() const {
  return m_clock.now_ms();
}

void account_health::prune_locked(std::int64_t now_ms) {
  for (auto slot = m_entries.begin(); slot != m_entries.end();) {
    if (now_ms > slot->second.m_prune_after_ms) {
      slot = m_entries.erase(slot);
    } else {
      ++slot;
    }
  }
}

} // namespace qoderz::server
