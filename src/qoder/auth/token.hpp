#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qoderz::qoder::auth {

using site_resolver = std::function<result<config::resolved_site>(std::string_view vpc)>;

inline constexpr std::chrono::minutes refresh_lead{5};
inline constexpr std::chrono::seconds spent_ttl{3600};
inline constexpr std::size_t max_spent_hops = 16;
inline constexpr std::chrono::milliseconds gate_poll_interval{25};

/// Keeps a stored account's tokens usable by refreshing them lazily, right before they are used.
///
/// A refresh token is single-use, so two concurrent refreshes of one account would leave one caller
/// holding a dead token. The service therefore lets exactly one coroutine per account refresh while
/// the others wait on a gate and pick up the result, and it remembers which spent refresh token
/// produced which record so a caller that never saw the write still does not refresh again.
///
/// The mutex only guards the gate table and the spent map; it is never held across a `co_await`,
/// and the HTTP work runs with no lock at all.
class token_service {
public:
  token_service(net::http_client& http, config::credential_store& store, site_resolver resolve,
                clock& time);

  /// The account with a token that stays valid for at least five minutes.
  ///
  /// An already fresh `current` comes back untouched — no disk read, no request. Otherwise the
  /// account is refreshed at most once for every concurrent caller and the result is stored. A
  /// record that cannot be written because the account was deleted is still returned, because the
  /// caller's token works even when the file no longer lists the account.
  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  ensure_fresh(config::account_record const& current);

private:
  struct gate {
    std::string m_key;
    bool m_busy = false;
  };

  struct spent_entry {
    std::string m_refresh;
    config::account_record m_record;
    std::chrono::steady_clock::time_point m_at;
  };

  /// Frees the gate however the refresh ends, including an early return or an escaping exception.
  class gate_guard {
  public:
    gate_guard(token_service& owner, std::shared_ptr<gate> held);
    ~gate_guard();
    gate_guard(gate_guard const&) = delete;
    gate_guard(gate_guard&&) = delete;
    gate_guard& operator=(gate_guard const&) = delete;
    gate_guard& operator=(gate_guard&&) = delete;

  private:
    token_service& m_owner;
    std::shared_ptr<gate> m_held;
  };

  /// The gate when this caller became the refresher, nullptr when somebody else already is.
  [[nodiscard]] std::shared_ptr<gate> try_enter(std::string const& account_key);
  void leave(std::string const& account_key);

  void remember_spent(std::string const& account_key, std::string const& spent_refresh,
                      config::account_record const& record);
  [[nodiscard]] std::optional<config::account_record>
  lookup_spent(std::string const& account_key, std::string const& refresh) const;

  [[nodiscard]] bool is_fresh(config::account_record const& record) const;

  /// The account as stored right now, or `working` itself when the account is gone.
  [[nodiscard]] result<config::account_record> reload(config::account_record const& working) const;

  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  refresh(config::account_record const& current);

  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  refresh_device(config::account_record const& current, config::resolved_site const& resolved);

  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  refresh_job(config::account_record const& current, config::resolved_site const& resolved);

  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  refresh_with_pat(config::account_record const& current, config::resolved_site const& resolved);

  [[nodiscard]] boost::asio::awaitable<result<void>> store(config::account_record const& record);

  net::http_client& m_http;
  config::credential_store& m_store;
  site_resolver m_resolve;
  clock& m_time;

  mutable std::mutex m_mutex;
  std::unordered_map<std::string, std::shared_ptr<gate>> m_gates;
  std::unordered_map<std::string, std::vector<spent_entry>> m_spent;
};

} // namespace qoderz::qoder::auth
