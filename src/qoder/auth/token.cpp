#include "qoder/auth/token.hpp"

#include "common/log.hpp"
#include "qoder/auth/pat.hpp"
#include "qoder/auth/support/json_http.hpp"
#include "qoder/errors.hpp"

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// GCC 14 pairs Asio's awaitable-frame `std::aligned_alloc` with `std::free` and then warns about
// the mismatch, which is the allocation pair Asio itself uses and a legal one. The warning fires
// only for these three coroutines because they are the ones GCC inlines to that size; silencing it
// here keeps the rest of the translation unit checked.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace qoderz::qoder::auth {
namespace {

namespace asio = boost::asio;

constexpr std::string_view device_refresh_path = "/api/v1/deviceToken/refresh";
constexpr std::string_view job_refresh_path = "/api/v1/jobToken/refresh";
constexpr std::string_view expired_message = "sign-in has expired — sign in again";
constexpr std::string_view no_job_token_message = "token refresh: Qoder returned no job token";
constexpr std::string_view no_device_token_message =
    "token refresh: Qoder returned no device token";
constexpr std::chrono::milliseconds job_expires_unit{1};
constexpr std::chrono::milliseconds device_expires_unit{1000};

bool is_auth_status(int status) noexcept {
  return status == 401 || status == 403;
}

error make_expired() {
  return make_error(error_kind::auth, std::string{expired_message});
}

boost::asio::awaitable<void> wait_for(std::chrono::milliseconds duration) {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer{executor, duration};
  co_await timer.async_wait(asio::use_awaitable);
}

boost::asio::awaitable<result<json_exchange>> post_refresh(net::http_client& http, std::string url,
                                                           std::string const& refresh_token) {
  json const body = {{"refresh_token", refresh_token}};
  co_return co_await send_json(http, json_post(std::move(url), body));
}

} // namespace

token_service::gate_guard::gate_guard(token_service& owner, std::shared_ptr<gate> held)
    : m_owner{owner}, m_held{std::move(held)} {}

token_service::gate_guard::~gate_guard() {
  if (m_held) {
    m_owner.leave(m_held->m_key);
  }
}

token_service::token_service(net::http_client& http, config::credential_store& store,
                             site_resolver resolve, clock& time)
    : m_http{http}, m_store{store}, m_resolve{std::move(resolve)}, m_time{time} {}

std::shared_ptr<token_service::gate> token_service::try_enter(std::string const& account_key) {
  std::lock_guard const lock(m_mutex);
  auto& entry = m_gates[account_key];
  if (!entry) {
    entry = std::make_shared<gate>();
    entry->m_key = account_key;
  }
  if (entry->m_busy) {
    return nullptr;
  }
  entry->m_busy = true;
  return entry;
}

void token_service::leave(std::string const& account_key) {
  std::lock_guard const lock(m_mutex);
  m_gates.erase(account_key);
}

void token_service::remember_spent(std::string const& account_key, std::string const& spent_refresh,
                                   config::account_record const& record) {
  if (spent_refresh.empty()) {
    return;
  }
  std::lock_guard const lock(m_mutex);
  auto const now = std::chrono::steady_clock::now();
  auto& entries = m_spent[account_key];
  entries.erase(
      std::remove_if(entries.begin(), entries.end(),
                     [now](spent_entry const& entry) { return now - entry.m_at > spent_ttl; }),
      entries.end());
  entries.push_back(spent_entry{.m_refresh = spent_refresh, .m_record = record, .m_at = now});
  while (entries.size() > max_spent_hops) {
    entries.erase(entries.begin());
  }
}

std::optional<config::account_record>
token_service::lookup_spent(std::string const& account_key, std::string const& refresh) const {
  if (refresh.empty()) {
    return std::nullopt;
  }
  std::lock_guard const lock(m_mutex);
  auto const found = m_spent.find(account_key);
  if (found == m_spent.end()) {
    return std::nullopt;
  }
  auto const now = std::chrono::steady_clock::now();
  for (auto const& entry : found->second) {
    if (entry.m_refresh == refresh && now - entry.m_at <= spent_ttl) {
      return entry.m_record;
    }
  }
  return std::nullopt;
}

bool token_service::is_fresh(config::account_record const& record) const {
  auto const lead = std::chrono::duration_cast<std::chrono::milliseconds>(refresh_lead).count();
  return record.m_expires_at - m_time.now_ms() > lead;
}

result<config::account_record> token_service::reload(config::account_record const& working) const {
  auto loaded = m_store.load();
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  auto const found = loaded->m_accounts.find(working.key());
  if (found == loaded->m_accounts.end()) {
    return working;
  }
  return found->second;
}

boost::asio::awaitable<result<void>> token_service::store(config::account_record const& record) {
  auto loaded = m_store.load();
  if (!loaded) {
    co_return std::unexpected(loaded.error());
  }
  auto const found = loaded->m_accounts.find(record.key());
  if (found == loaded->m_accounts.end()) {
    log(log_level::warn, "the refreshed token could not be saved; the account is gone",
        {{"site", record.m_site}, {"uid", record.m_uid}});
    co_return result<void>{};
  }
  found->second = record;
  co_return m_store.save(*loaded);
}

boost::asio::awaitable<result<config::account_record>>
token_service::ensure_fresh(config::account_record const& current) {
  auto const account_key = current.key();
  auto working = current;

  for (;;) {
    if (is_fresh(working)) {
      co_return working;
    }
    if (auto const remembered = lookup_spent(account_key, working.m_refresh_token);
        remembered.has_value()) {
      co_return *remembered;
    }

    auto held = try_enter(account_key);
    if (!held) {
      co_await wait_for(gate_poll_interval);
      auto disk = reload(working);
      if (!disk) {
        co_return std::unexpected(disk.error());
      }
      working = std::move(*disk);
      continue;
    }

    gate_guard guard{*this, held};
    auto disk = reload(working);
    if (!disk) {
      co_return std::unexpected(disk.error());
    }
    if (disk->m_access_token != working.m_access_token) {
      working = std::move(*disk);
      if (is_fresh(working)) {
        co_return working;
      }
      if (auto const remembered = lookup_spent(account_key, working.m_refresh_token);
          remembered.has_value()) {
        co_return *remembered;
      }
    }

    auto next = co_await refresh(working);
    if (!next) {
      co_return std::unexpected(next.error());
    }
    remember_spent(account_key, working.m_refresh_token, *next);
    auto const saved = co_await store(*next);
    if (!saved) {
      co_return std::unexpected(saved.error());
    }
    log(log_level::debug, "refreshed the account token",
        {{"site", next->m_site}, {"uid", next->m_uid}});
    co_return *next;
  }
}

boost::asio::awaitable<result<config::account_record>>
token_service::refresh(config::account_record const& current) {
  auto resolved = m_resolve(current.m_vpc);
  if (!resolved) {
    co_return std::unexpected(resolved.error());
  }
  if (current.m_device_chat) {
    co_return co_await refresh_device(current, *resolved);
  }
  co_return co_await refresh_job(current, *resolved);
}

boost::asio::awaitable<result<config::account_record>>
token_service::refresh_device(config::account_record const& current,
                              config::resolved_site const& resolved) {
  auto sent = co_await post_refresh(
      m_http, resolved.m_openapi_base + std::string{device_refresh_path}, current.m_refresh_token);
  if (!sent) {
    co_return std::unexpected(sent.error());
  }
  if (is_auth_status(sent->m_status)) {
    log(log_level::warn, "the device refresh token was refused",
        {{"site", current.m_site}, {"http_status", std::to_string(sent->m_status)}});
    co_return std::unexpected(make_expired());
  }
  if (sent->m_status != 200) {
    co_return std::unexpected(explain(*sent));
  }

  auto const device_token = read_text(sent->m_body, {"token", "device_token"});
  if (device_token.empty()) {
    co_return std::unexpected(
        make_error(error_kind::upstream, std::string{no_device_token_message}));
  }
  auto const device_refresh = read_text(sent->m_body, {"refresh_token", "refreshToken"});

  // A device account mirrors the two pairs: `access` is the device token and `refresh` is the
  // device refresh token, so they are written together. When the server does not rotate the
  // refresh token the one that was just accepted stays in force.
  auto const refresh = device_refresh.empty() ? current.m_refresh_token : device_refresh;
  auto next = current;
  next.m_device_token = device_token;
  next.m_device_refresh_token = refresh;
  next.m_access_token = device_token;
  next.m_refresh_token = refresh;
  next.m_expires_at = read_expires_at(sent->m_body, m_time.now_ms(), device_expires_unit);
  next.m_device_chat = true;
  co_return next;
}

boost::asio::awaitable<result<config::account_record>>
token_service::refresh_job(config::account_record const& current,
                           config::resolved_site const& resolved) {
  auto sent = co_await post_refresh(m_http, resolved.m_openapi_base + std::string{job_refresh_path},
                                    current.m_refresh_token);
  if (!sent) {
    co_return std::unexpected(sent.error());
  }
  if (is_auth_status(sent->m_status)) {
    log(log_level::warn, "the job refresh token was refused",
        {{"site", current.m_site}, {"http_status", std::to_string(sent->m_status)}});
    if (current.m_pat_auth && !current.m_pat.empty()) {
      co_return co_await refresh_with_pat(current, resolved);
    }
    co_return std::unexpected(make_expired());
  }
  if (sent->m_status != 200) {
    co_return std::unexpected(explain(*sent));
  }

  auto const access = read_text(sent->m_body, {"token", "access_token"});
  auto const refresh = read_text(sent->m_body, {"refresh_token", "refreshToken"});
  if (access.empty() || refresh.empty()) {
    co_return std::unexpected(make_error(error_kind::upstream, std::string{no_job_token_message}));
  }

  auto next = current;
  next.m_access_token = access;
  next.m_refresh_token = refresh;
  next.m_expires_at = read_expires_at(sent->m_body, m_time.now_ms(), job_expires_unit);
  next.m_device_chat = false;
  co_return next;
}

boost::asio::awaitable<result<config::account_record>>
token_service::refresh_with_pat(config::account_record const& current,
                                config::resolved_site const& resolved) {
  auto site = config::find_site(resolved.m_site_id);
  if (!site) {
    co_return std::unexpected(site.error());
  }
  auto renewed = co_await login_with_pat(m_http, **site, resolved, current.m_pat, m_time);
  if (!renewed) {
    co_return std::unexpected(renewed.error());
  }

  auto next = *renewed;
  next.m_site = current.m_site;
  next.m_name = current.m_name;
  next.m_machine_id = current.m_machine_id;
  next.m_created_at = current.m_created_at;
  next.m_device_token = current.m_device_token;
  next.m_device_refresh_token = current.m_device_refresh_token;
  next.m_device_chat = current.m_device_chat;
  next.m_pat = current.m_pat;
  next.m_pat_auth = true;
  co_return next;
}

} // namespace qoderz::qoder::auth
