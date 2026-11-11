#pragma once

#include "common/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qoderz::server {

/// The only upstream failures that take an account out of rotation, per `docs/design/server.md`
/// §6.2. A busy queue, a malformed request, an upstream 5xx answer and a transport failure are
/// deliberately absent: they never cool an account down and never trigger failover.
enum class failure_kind { auth, quota };

[[nodiscard]] constexpr std::string_view to_string(failure_kind kind) noexcept {
  switch (kind) {
  case failure_kind::auth:
    return "auth";
  case failure_kind::quota:
    return "quota";
  }
  return "unknown";
}

/// The cooldown remembered for one account: the failure that caused it, how many times the account
/// failed in a row since its last success, and when the cooldown ends in Unix milliseconds.
struct health_state {
  failure_kind m_kind = failure_kind::quota;
  int m_failures = 0;
  std::int64_t m_cooldown_until_ms = 0;
};

/// Per-account cooldown table of `docs/design/server.md` §6.2.
///
/// The first quota failure cools the account for 5 minutes and the first auth failure for 15; every
/// further *consecutive* failure of the same kind doubles the window, capped at one hour, so quota
/// runs 5, 10, 20, 40, 60, 60… and auth runs 15, 30, 60, 60…. A different kind restarts the streak
/// at one, because an exhausted quota does not make a broken token worse. A success clears the
/// entry and its streak. An entry stays remembered for `entry_retention_ms` past the end of its
/// cooldown so a flapping account keeps its streak instead of dropping back to the base window.
///
/// Every member is safe to call from several threads. The mutex only guards the table; nothing else
/// runs under it.
class account_health {
public:
  explicit account_health(clock& time);

  /// Records one failure of `account_key` and restarts its cooldown from now.
  void report_failure(std::string_view account_key, failure_kind kind);

  /// Clears the entry, including the streak: the account is back to the base window.
  void report_success(std::string_view account_key);

  [[nodiscard]] bool is_cooling(std::string_view account_key) const;

  /// The recorded entry, also while it is only retained after its cooldown ended; `std::nullopt`
  /// when the account has no entry at all. Use `is_cooling` to tell the two apart.
  [[nodiscard]] std::optional<health_state> state(std::string_view account_key) const;

  /// How many entries are cooling right now; retained but expired entries are not counted.
  [[nodiscard]] std::size_t cooling_count() const;

  /// True when at least one entry is cooling and every cooling entry is an auth failure. It answers
  /// the 401-or-429 question for a pool that rotates over *every* account; `account_pool` derives
  /// the same answer from its considered accounts instead, because a pinned or subset pool must
  /// ignore the cooldowns of accounts it does not rotate over.
  [[nodiscard]] bool all_cooling_auth() const;

  /// Drops entries whose cooldown ended more than `entry_retention_ms` ago. `report_failure` and
  /// `report_success` do this for free; the explicit call is for a pool that stays idle.
  void prune();

  /// The shared wall clock; `account_pool` reads it so bindings and cooldowns share one time
  /// source instead of the caller threading two clocks around.
  [[nodiscard]] std::int64_t now_ms() const;

  static constexpr std::int64_t quota_base_ms = 5 * 60 * 1000;
  static constexpr std::int64_t auth_base_ms = 15 * 60 * 1000;
  static constexpr std::int64_t max_cooldown_ms = 60 * 60 * 1000;
  static constexpr std::int64_t entry_retention_ms = 2 * 60 * 60 * 1000;

  /// The longest doubling step that can matter before the one hour cap applies.
  static constexpr int max_doubling_step = 16;

  /// `min(base << (failures - 1), 1h)`, with the shift clamped so a long streak cannot overflow.
  [[nodiscard]] static constexpr std::int64_t cooldown_ms(failure_kind kind,
                                                          int failures) noexcept {
    std::int64_t base = quota_base_ms;
    if (kind == failure_kind::auth) {
      base = auth_base_ms;
    }
    int step = failures - 1;
    if (step < 0) {
      step = 0;
    }
    if (step > max_doubling_step) {
      step = max_doubling_step;
    }
    auto const doubled = base << step;
    return doubled < max_cooldown_ms ? doubled : max_cooldown_ms;
  }

private:
  struct entry {
    health_state m_state;
    std::int64_t m_prune_after_ms = 0;
  };

  void prune_locked(std::int64_t now_ms);

  clock& m_clock;
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, entry> m_entries;
};

} // namespace qoderz::server
