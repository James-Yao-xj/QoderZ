#pragma once

#include "common/error.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "server/pool/health.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::server {

class session_affinity;

/// One account of a pool: the credential snapshot `serve` took at startup, plus the upstream bases
/// of its VPC instance. `m_key` is always `"{site}/{name}"`, the same key the credential file uses.
struct pool_account {
  std::string m_key;
  config::account_record m_account;
  config::resolved_site m_site;
};

/// How the pool was narrowed on the command line, per `docs/design/server.md` §6.1. `m_pinned_key`
/// and `m_only_keys` are mutually exclusive; `m_ignore_cooldown` is the explicit escape hatch that
/// keeps a cooling account in rotation.
struct pool_options {
  std::optional<std::string> m_pinned_key;
  std::vector<std::string> m_only_keys;
  bool m_ignore_cooldown = false;
};

inline constexpr std::string_view not_signed_in_message = "not signed in; run: qoderz login";
inline constexpr std::string_view all_unavailable_message =
    "all Qoder accounts are temporarily unavailable; retry later";

/// Candidate selection of `docs/design/server.md` §6.1 over one site's accounts.
///
/// Accounts are kept in the base order of §6.1 — by name ignoring ASCII case — and every candidate
/// list is a filtered view of that order. `rotate = false` is what `/v1/models` uses: it neither
/// reads nor writes the session binding and does not advance the round-robin counter. `rotate =
/// true` advances the process-wide counter by one per call, so new sessions start at different
/// accounts, and puts the session's healthy bound account first.
///
/// The class only reads the shared health table and binding table; it never mutates them itself.
/// Returned pointers are owned by the pool and stay valid for the pool's lifetime: a `serve`
/// process never changes its account snapshot.
class account_pool {
public:
  /// Builds a pool. Accounts from more than one site, a pinned key or a subset key that names no
  /// account, a pinned pool that also carries a subset, and duplicate keys are configuration
  /// errors. An empty account list is accepted: the caller decides whether that is fatal at
  /// startup, and the pool answers with `not_signed_in_message` at request time.
  static result<account_pool> create(std::vector<pool_account> accounts, account_health& health,
                                     session_affinity& affinity, pool_options options = {});

  /// Validates `options` against `accounts`, sorts them into base order and returns the site id
  /// they share.
  static result<std::string> prepare(std::vector<pool_account>& accounts,
                                     pool_options const& options);

  /// Assembles an already validated pool. `pool` uses it after owning the two tables, because a
  /// `std::expected` result cannot reach a private constructor; go through `create` otherwise.
  account_pool(std::vector<pool_account> accounts, account_health& health,
               session_affinity& affinity, pool_options options, std::string site_id);
  account_pool(account_pool const&) = delete;
  account_pool(account_pool&&) = delete;
  account_pool& operator=(account_pool const&) = delete;
  account_pool& operator=(account_pool&&) = delete;

  /// Healthy accounts in the order the failover loop should try them, empty when every considered
  /// account is cooling. `rotate = true` also touches the session binding of `session_id`.
  [[nodiscard]] std::vector<pool_account const*> candidates(std::string_view session_id,
                                                            bool rotate);

  void report_success(std::string_view account_key);
  void report_failure(std::string_view account_key, failure_kind kind);

  /// Remembers `account_key` as the account of `session_id`; the server layer calls it only after
  /// an upstream open succeeded.
  void bind(std::string_view session_id, std::string_view account_key);

  [[nodiscard]] bool empty() const noexcept;

  /// How many accounts this pool rotates over: the pinned account, the subset, or the whole
  /// snapshot. It is the denominator of the fully-cooled check, so a pinned pool has size one.
  [[nodiscard]] std::size_t size() const;

  /// How many of those accounts are cooling right now, whether or not `m_ignore_cooldown` is set.
  [[nodiscard]] std::size_t cooling_count() const;

  /// The account keys this pool rotates over, in base order.
  [[nodiscard]] std::vector<std::string> keys() const;

  /// The error to return when `candidates` came back empty: 401 for a pool without accounts, and
  /// for a fully cooled one 401 when every cooldown is an auth failure and 429 otherwise.
  [[nodiscard]] error no_candidates_error() const;

  [[nodiscard]] std::string const& site_id() const noexcept;

private:
  [[nodiscard]] std::vector<pool_account const*> considered_accounts() const;

  std::vector<pool_account> m_accounts;
  account_health& m_health;
  session_affinity& m_affinity;
  pool_options m_options;
  std::string m_site_id;
  std::size_t m_rotation = 0;
  mutable std::mutex m_mutex;
};

} // namespace qoderz::server
