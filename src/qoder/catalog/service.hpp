#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/catalog/model.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qoderz::qoder::catalog {

inline constexpr std::chrono::minutes cache_ttl{5};
inline constexpr std::size_t max_cache_entries = 64;

/// Fetches the model listing of one account and remembers it for five minutes.
///
/// A cached listing is keyed by `site + "\0" + vpc + "\0" + uid`: the same account on the same VPC
/// endpoint is the only combination whose models are interchangeable. An empty listing is not
/// cached — it means the account cannot see the catalogue right now, and the built-in list of the
/// site answers instead of an empty model list.
///
/// The mutex guards the cache only and is never held across a `co_await`; the network work runs
/// unlocked. Concurrent callers for the same key may therefore fetch twice on a cold cache, which
/// costs one request and never corrupts the map.
class catalog_service {
public:
  catalog_service(net::http_client& http, clock& time);

  /// The models of `account`, from cache, network or the site's built-in list.
  ///
  /// `force_refresh` skips the cache read but still stores a successful result. A non-200 answer is
  /// classified by the protocol table; a 200 with an unusable body is treated like an empty
  /// listing, so a broken listing degrades to the built-in models instead of failing the command.
  [[nodiscard]] boost::asio::awaitable<result<std::vector<model_info>>>
  fetch(config::resolved_site const& site, config::account_record const& account,
        bool force_refresh);

private:
  struct cache_entry {
    std::vector<model_info> m_models;
    std::int64_t m_expires_at = 0;
  };

  [[nodiscard]] static std::string cache_key(config::resolved_site const& site,
                                             config::account_record const& account);

  [[nodiscard]] std::optional<std::vector<model_info>> read_cache(std::string const& key,
                                                                  std::int64_t now_ms);

  void store_cache(std::string const& key, std::vector<model_info> models, std::int64_t now_ms);

  /// Drops every entry whose TTL has elapsed; called with `m_mutex` held.
  void prune_locked(std::int64_t now_ms);

  /// Drops the entry that expires first; called with `m_mutex` held and only when the cache is
  /// full.
  void drop_soonest_locked();

  net::http_client& m_http;
  clock& m_time;

  std::mutex m_mutex;
  std::unordered_map<std::string, cache_entry> m_cache;
};

} // namespace qoderz::qoder::catalog
