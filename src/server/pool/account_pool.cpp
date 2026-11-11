#include "server/pool/account_pool.hpp"

#include "common/strings.hpp"
#include "server/pool/affinity.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace qoderz::server {
namespace {

int compare_ignore_case(std::string_view left, std::string_view right) noexcept {
  auto const shared = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < shared; ++index) {
    auto const left_value = strings::ascii_lower(left[index]);
    auto const right_value = strings::ascii_lower(right[index]);
    if (left_value != right_value) {
      return left_value < right_value ? -1 : 1;
    }
  }
  if (left.size() == right.size()) {
    return 0;
  }
  return left.size() < right.size() ? -1 : 1;
}

std::string_view effective_site(pool_account const& account) noexcept {
  return account.m_site.m_site_id.empty() ? std::string_view{account.m_account.m_site}
                                          : std::string_view{account.m_site.m_site_id};
}

std::string lowered_key(std::string_view key) {
  return strings::ascii_lower_copy(key);
}

} // namespace

result<std::string> account_pool::prepare(std::vector<pool_account>& accounts,
                                          pool_options const& options) {
  if (options.m_pinned_key.has_value() && !options.m_only_keys.empty()) {
    return std::unexpected(configuration_error(
        "A pinned account and an account subset cannot be combined in one pool."));
  }

  std::string site_id;
  if (!accounts.empty()) {
    site_id = std::string{effective_site(accounts.front())};
  }

  std::set<std::string, std::less<>> seen;
  for (auto const& account : accounts) {
    auto const site = effective_site(account);
    if (site != site_id) {
      return std::unexpected(configuration_error("Accounts of one pool must belong to one site: '" +
                                                 site_id + "' and '" + std::string{site} +
                                                 "' were mixed, for key '" + account.m_key + "'."));
    }
    if (!seen.emplace(lowered_key(account.m_key)).second) {
      return std::unexpected(
          configuration_error("Duplicate account key '" + account.m_key + "' in one pool."));
    }
  }

  std::stable_sort(
      accounts.begin(), accounts.end(), [](pool_account const& left, pool_account const& right) {
        auto const ordered = compare_ignore_case(left.m_account.m_name, right.m_account.m_name);
        if (ordered != 0) {
          return ordered < 0;
        }
        return left.m_key < right.m_key;
      });

  auto const exists = [&accounts](std::string_view key) {
    return std::any_of(accounts.begin(), accounts.end(),
                       [key](pool_account const& account) { return account.m_key == key; });
  };

  if (options.m_pinned_key.has_value() && !exists(*options.m_pinned_key)) {
    return std::unexpected(configuration_error("The pinned account '" + *options.m_pinned_key +
                                               "' is not in the account pool."));
  }
  for (auto const& key : options.m_only_keys) {
    if (!exists(key)) {
      return std::unexpected(configuration_error("The account subset names '" + key +
                                                 "', which is not in the account pool."));
    }
  }

  return site_id;
}

result<account_pool> account_pool::create(std::vector<pool_account> accounts,
                                          account_health& health, session_affinity& affinity,
                                          pool_options options) {
  auto site_id = prepare(accounts, options);
  if (!site_id) {
    return std::unexpected(std::move(site_id.error()));
  }
  return result<account_pool>{std::in_place, std::move(accounts), health,
                              affinity,      std::move(options),  std::move(*site_id)};
}

account_pool::account_pool(std::vector<pool_account> accounts, account_health& health,
                           session_affinity& affinity, pool_options options, std::string site_id)
    : m_accounts{std::move(accounts)}, m_health{health}, m_affinity{affinity},
      m_options{std::move(options)}, m_site_id{std::move(site_id)} {}

std::vector<pool_account const*> account_pool::considered_accounts() const {
  std::vector<pool_account const*> picked;

  if (m_options.m_pinned_key.has_value()) {
    for (auto const& account : m_accounts) {
      if (account.m_key == *m_options.m_pinned_key) {
        picked.push_back(&account);
        break;
      }
    }
    return picked;
  }

  if (m_options.m_only_keys.empty()) {
    for (auto const& account : m_accounts) {
      picked.push_back(&account);
    }
    return picked;
  }

  for (auto const& account : m_accounts) {
    auto const listed =
        std::find(m_options.m_only_keys.begin(), m_options.m_only_keys.end(), account.m_key);
    if (listed != m_options.m_only_keys.end()) {
      picked.push_back(&account);
    }
  }
  return picked;
}

std::vector<pool_account const*> account_pool::candidates(std::string_view session_id,
                                                          bool rotate) {
  std::vector<pool_account const*> healthy;
  for (auto const* account : considered_accounts()) {
    if (m_options.m_ignore_cooldown || !m_health.is_cooling(account->m_key)) {
      healthy.push_back(account);
    }
  }

  if (!rotate) {
    return healthy;
  }

  std::size_t offset = 0;
  {
    std::lock_guard<std::mutex> guard{m_mutex};
    offset = m_rotation++;
  }

  if (healthy.size() > 1) {
    auto const start = static_cast<std::ptrdiff_t>(offset % healthy.size());
    std::rotate(healthy.begin(), healthy.begin() + start, healthy.end());
  }

  auto const bound_key = m_affinity.bound(session_id, m_health.now_ms());
  if (!bound_key.has_value()) {
    return healthy;
  }

  auto const bound_slot =
      std::find_if(healthy.begin(), healthy.end(), [&bound_key](pool_account const* account) {
        return account->m_key == *bound_key;
      });
  if (bound_slot != healthy.end() && bound_slot != healthy.begin()) {
    std::rotate(healthy.begin(), bound_slot, bound_slot + 1);
  }
  return healthy;
}

void account_pool::report_success(std::string_view account_key) {
  m_health.report_success(account_key);
}

void account_pool::report_failure(std::string_view account_key, failure_kind kind) {
  m_health.report_failure(account_key, kind);
}

void account_pool::bind(std::string_view session_id, std::string_view account_key) {
  m_affinity.bind(session_id, std::string{account_key}, m_health.now_ms());
}

bool account_pool::empty() const noexcept {
  return m_accounts.empty();
}

std::size_t account_pool::size() const {
  return considered_accounts().size();
}

std::size_t account_pool::cooling_count() const {
  std::size_t count = 0;
  for (auto const* account : considered_accounts()) {
    if (m_health.is_cooling(account->m_key)) {
      ++count;
    }
  }
  return count;
}

std::vector<std::string> account_pool::keys() const {
  auto const considered = considered_accounts();
  std::vector<std::string> result;
  result.reserve(considered.size());
  for (auto const* account : considered) {
    result.push_back(account->m_key);
  }
  return result;
}

error account_pool::no_candidates_error() const {
  auto const considered = considered_accounts();
  auto const no_account = auth_error(std::string{not_signed_in_message});
  if (considered.empty()) {
    return no_account;
  }

  bool any_available = false;
  bool all_auth = true;
  for (auto const* account : considered) {
    if (m_options.m_ignore_cooldown || !m_health.is_cooling(account->m_key)) {
      any_available = true;
      continue;
    }
    auto const recorded = m_health.state(account->m_key);
    if (!recorded.has_value() || recorded->m_kind != failure_kind::auth) {
      all_auth = false;
    }
  }

  if (any_available) {
    return no_account;
  }
  if (all_auth) {
    return auth_error(std::string{all_unavailable_message});
  }
  return quota_error(std::string{all_unavailable_message});
}

std::string const& account_pool::site_id() const noexcept {
  return m_site_id;
}

} // namespace qoderz::server
