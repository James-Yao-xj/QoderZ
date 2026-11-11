#include "qoder/catalog/service.hpp"

#include "common/json.hpp"
#include "common/log.hpp"
#include "common/net/http_types.hpp"
#include "protocol/cosy_signer.hpp"
#include "qoder/catalog/listing.hpp"
#include "qoder/errors.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qoderz::qoder::catalog {
namespace {

constexpr std::string_view listing_path = "/algo/api/v2/model/list?Encode=1";
constexpr int http_ok = 200;

/// The TTL in the same unit the clock speaks, so `cache_ttl` stays readable in minutes.
constexpr std::int64_t cache_ttl_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(cache_ttl).count();

/// The embedded RSA key is parsed once per process; the wrapper is immutable after that, so sharing
/// it across every catalog call is both safe and cheaper than re-reading the PEM per fetch.
result<protocol::rsa_key_wrapper const*> shared_wrapper() {
  static std::unique_ptr<protocol::pem_rsa_key_wrapper> const wrapper = [] {
    auto created = protocol::pem_rsa_key_wrapper::create();
    return created.has_value() ? std::move(*created) : nullptr;
  }();
  if (!wrapper) {
    return std::unexpected(
        make_error(error_kind::internal, "cosy: cannot load the embedded signing key"));
  }
  return wrapper.get();
}

protocol::cosy_identity identity_of(config::account_record const& account) {
  protocol::cosy_identity identity;
  identity.m_uid = account.m_uid;
  identity.m_name = account.m_display_name.empty() ? account.m_name : account.m_display_name;
  identity.m_email = account.m_email;
  identity.m_access_token = account.m_access_token;
  identity.m_machine_id = account.m_machine_id;
  identity.m_machine_os = protocol::machine_os();
  return identity;
}

/// The member under `name`, or an empty value so a missing array can be passed straight to
/// `merge_listing` without a second branch.
json const& member_of(json const& object, std::string_view name) {
  static json const empty;
  if (!object.is_object()) {
    return empty;
  }
  auto const found = object.find(name);
  return found == object.end() ? empty : *found;
}

/// The models of a 200 body; a body that is not a JSON object is an empty listing, not a failure,
/// because the caller answers it with the site's built-in list either way.
std::vector<model_info> models_of(std::string_view body) {
  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return {};
  }
  return parse_models(merge_listing(member_of(parsed, "app"), member_of(parsed, "chat")));
}

} // namespace

catalog_service::catalog_service(net::http_client& http, clock& time)
    : m_http{http}, m_time{time} {}

std::string catalog_service::cache_key(config::resolved_site const& site,
                                       config::account_record const& account) {
  std::string key;
  key.reserve(site.m_site_id.size() + site.m_vpc.size() + account.m_uid.size() + 2);
  key.append(site.m_site_id);
  key.push_back('\0');
  key.append(site.m_vpc);
  key.push_back('\0');
  key.append(account.m_uid);
  return key;
}

std::optional<std::vector<model_info>> catalog_service::read_cache(std::string const& key,
                                                                   std::int64_t now_ms) {
  std::lock_guard const lock{m_mutex};
  auto const found = m_cache.find(key);
  if (found == m_cache.end()) {
    return std::nullopt;
  }
  if (now_ms >= found->second.m_expires_at) {
    m_cache.erase(found);
    return std::nullopt;
  }
  return found->second.m_models;
}

void catalog_service::store_cache(std::string const& key, std::vector<model_info> models,
                                  std::int64_t now_ms) {
  std::lock_guard const lock{m_mutex};
  if (!m_cache.contains(key) && m_cache.size() >= max_cache_entries) {
    prune_locked(now_ms);
    if (m_cache.size() >= max_cache_entries) {
      drop_soonest_locked();
    }
  }
  m_cache.insert_or_assign(
      key, cache_entry{.m_models = std::move(models), .m_expires_at = now_ms + cache_ttl_ms});
}

void catalog_service::prune_locked(std::int64_t now_ms) {
  for (auto entry = m_cache.begin(); entry != m_cache.end();) {
    if (now_ms >= entry->second.m_expires_at) {
      entry = m_cache.erase(entry);
    } else {
      ++entry;
    }
  }
}

void catalog_service::drop_soonest_locked() {
  auto oldest = m_cache.end();
  for (auto entry = m_cache.begin(); entry != m_cache.end(); ++entry) {
    if (oldest == m_cache.end() || entry->second.m_expires_at < oldest->second.m_expires_at) {
      oldest = entry;
    }
  }
  if (oldest != m_cache.end()) {
    m_cache.erase(oldest);
  }
}

boost::asio::awaitable<result<std::vector<model_info>>>
catalog_service::fetch(config::resolved_site const& site, config::account_record const& account,
                       bool force_refresh) {
  auto const key = cache_key(site, account);
  auto const now_ms = m_time.now_ms();

  if (!force_refresh) {
    if (auto cached = read_cache(key, now_ms); cached.has_value()) {
      log(log_level::debug, "model catalog served from cache",
          {{"site", site.m_site_id}, {"uid", account.m_uid}});
      co_return std::move(*cached);
    }
  }

  auto const wrapper = shared_wrapper();
  if (!wrapper) {
    co_return std::unexpected(wrapper.error());
  }

  std::string url{site.m_api_base};
  url.append(listing_path);

  auto material =
      protocol::create_cosy_headers(url, identity_of(account), {}, now_ms / 1000, **wrapper);
  if (!material) {
    co_return std::unexpected(std::move(material.error()));
  }

  net::http_request request;
  request.m_method = "GET";
  request.m_url = std::move(url);
  request.m_headers.reserve(material->m_headers.size());
  for (auto const& header : material->m_headers) {
    request.m_headers.push_back(
        net::http_header{.m_name = header.m_name, .m_value = header.m_value});
  }

  auto response = co_await m_http.send(request);
  if (!response) {
    co_return std::unexpected(std::move(response.error()));
  }

  if (response->m_status != http_ok) {
    log(log_level::warn, "model catalog request failed",
        {{"site", site.m_site_id},
         {"uid", account.m_uid},
         {"status", std::to_string(response->m_status)}});
    co_return std::unexpected(to_error(from_response(response->m_status, response->m_body)));
  }

  auto models = models_of(response->m_body);
  if (models.empty()) {
    log(log_level::debug, "model catalog is empty; using the built-in list",
        {{"site", site.m_site_id}, {"uid", account.m_uid}, {"status", std::to_string(http_ok)}});
    co_return fallback_models(site.m_site_id);
  }

  store_cache(key, models, m_time.now_ms());
  log(log_level::debug, "model catalog fetched",
      {{"site", site.m_site_id}, {"uid", account.m_uid}, {"status", std::to_string(http_ok)}});
  co_return models;
}

} // namespace qoderz::qoder::catalog
