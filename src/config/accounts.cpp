#include "config/accounts.hpp"

#include "common/strings.hpp"
#include "config/site.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::config {
namespace {

constexpr std::string_view fallback_name = "account";
constexpr std::size_t max_echoed_text = 80;

bool is_ascii_alnum(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

std::string truncate_name(std::string value, std::size_t limit) {
  if (value.size() <= limit) {
    return value;
  }
  value.resize(limit);
  while (!value.empty() && value.back() == '-') {
    value.pop_back();
  }
  return value;
}

std::string slug_from(std::string_view value) {
  std::string slug;
  slug.reserve(value.size());
  bool pending_separator = false;
  for (auto const character : value) {
    if (!is_ascii_alnum(character)) {
      pending_separator = true;
      continue;
    }
    if (pending_separator && !slug.empty()) {
      slug += '-';
    }
    pending_separator = false;
    slug += strings::ascii_lower(character);
  }
  return truncate_name(std::move(slug), max_account_name_length);
}

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

bool prefix_match(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() &&
         strings::equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

struct identity_slot {
  std::string_view identity_fields::* m_incoming;
  std::string account_record::* m_stored;
};

// The first non-empty incoming field decides the comparison and is matched against the same
// stored field, so a token that only carries an email still finds an account whose uid is set.
constexpr std::array<identity_slot, 4> identity_slots{{
    {.m_incoming = &identity_fields::m_uid, .m_stored = &account_record::m_uid},
    {.m_incoming = &identity_fields::m_account_id, .m_stored = &account_record::m_account_id},
    {.m_incoming = &identity_fields::m_email, .m_stored = &account_record::m_email},
    {.m_incoming = &identity_fields::m_display_name, .m_stored = &account_record::m_display_name},
}};

std::unexpected<error> invalid_name_error(std::string_view name) {
  auto const shown = name.size() > max_echoed_text ? name.substr(0, max_echoed_text) : name;
  return std::unexpected(configuration_error(
      "'" + std::string{shown} + "' is not a valid account name; use 1 to " +
      std::to_string(max_account_name_length) +
      " characters from A-Z, a-z, 0-9, '.', '_' or '-', starting with a letter or digit."));
}

std::unexpected<error> name_taken_error(std::string_view name, std::string_view site,
                                        std::string_view stored_name) {
  return std::unexpected(configuration_error(
      "Account '" + std::string{name} + "' already exists for site '" + std::string{site} +
      "'; use another name with --name, or run: qoderz logout --account " +
      std::string{stored_name}));
}

std::unexpected<error> no_accounts_error(std::string_view site) {
  return std::unexpected(configuration_error("No accounts are stored for site '" +
                                             std::string{site} + "'; run: qoderz login"));
}

std::string join_names(std::vector<account_record const*> const& accounts) {
  std::string names;
  for (auto const* account : accounts) {
    if (!names.empty()) {
      names += ", ";
    }
    names += account->m_name;
  }
  return names;
}

bool same_name(account_record const& account, std::string_view name) noexcept {
  return strings::equals_ignore_case(account.m_name, name);
}

account_record* find_by_name(credential_file& file, std::string_view site, std::string_view name) {
  for (auto& entry : file.m_accounts) {
    if (entry.second.m_site == site && same_name(entry.second, name)) {
      return &entry.second;
    }
  }
  return nullptr;
}

bool is_default(credential_file const& file, std::string_view site, std::string_view key) {
  auto const found = file.m_defaults.find(normalize_site_id(site));
  return found != file.m_defaults.end() && found->second == key;
}

} // namespace

result<void> validate_account_name(std::string_view name) {
  auto const trimmed = strings::trim(name);
  if (is_valid_account_name(trimmed)) {
    return {};
  }
  return invalid_name_error(trimmed);
}

std::string slug_account_name(std::string_view email, std::string_view display_name,
                              std::string_view uid) {
  auto const at = email.find('@');
  auto const local_part = at == std::string_view::npos ? email : email.substr(0, at);
  for (auto const source : {local_part, display_name, uid}) {
    auto slug = slug_from(source);
    if (!slug.empty()) {
      return slug;
    }
  }
  return std::string{fallback_name};
}

std::string unique_account_name(std::string_view base,
                                std::span<std::string const> existing_names) {
  std::set<std::string, std::less<>> taken;
  for (auto const& name : existing_names) {
    taken.insert(strings::ascii_lower_copy(name));
  }
  auto const start = base.empty() ? std::string{fallback_name} : std::string{base};
  if (!taken.contains(strings::ascii_lower_copy(start))) {
    return start;
  }
  for (std::size_t index = 2;; ++index) {
    auto const suffix = "-" + std::to_string(index);
    auto candidate = truncate_name(start, max_account_name_length - suffix.size()) + suffix;
    if (!taken.contains(strings::ascii_lower_copy(candidate))) {
      return candidate;
    }
  }
}

std::vector<account_record const*> accounts_for_site(credential_file const& file,
                                                     std::string_view site) {
  auto const site_id = normalize_site_id(site);
  std::vector<account_record const*> accounts;
  for (auto const& entry : file.m_accounts) {
    if (entry.second.m_site == site_id) {
      accounts.push_back(&entry.second);
    }
  }
  std::sort(accounts.begin(), accounts.end(),
            [](account_record const* left, account_record const* right) {
              auto const order = compare_ignore_case(left->m_name, right->m_name);
              return order != 0 ? order < 0 : left->key() < right->key();
            });
  return accounts;
}

result<account_record*> find_by_identity(credential_file& file, std::string_view site,
                                         std::string_view vpc, identity_fields const& identity) {
  auto const site_id = normalize_site_id(site);
  auto const wanted_vpc = strings::trim(vpc);
  for (auto const& slot : identity_slots) {
    auto const wanted = strings::trim(identity.*slot.m_incoming);
    if (wanted.empty()) {
      continue;
    }
    for (auto& entry : file.m_accounts) {
      auto& account = entry.second;
      if (account.m_site != site_id || strings::trim(account.m_vpc) != wanted_vpc) {
        continue;
      }
      if (strings::trim(account.*slot.m_stored) == wanted) {
        return &account;
      }
    }
    return nullptr;
  }
  return nullptr;
}

result<account_record*> upsert_account(credential_file& file, account_record account,
                                       std::optional<std::string> requested_name,
                                       bool replace_existing_name) {
  account.m_site = normalize_site_id(account.m_site);
  account.m_vpc = std::string{strings::trim(account.m_vpc)};
  account.m_uid = std::string{strings::trim(account.m_uid)};
  account.m_account_id = std::string{strings::trim(account.m_account_id)};
  account.m_email = std::string{strings::trim(account.m_email)};
  account.m_display_name = std::string{strings::trim(account.m_display_name)};

  if (account.m_site.empty()) {
    return std::unexpected(configuration_error("An account needs a site id."));
  }
  if (!is_valid_site_id(account.m_site)) {
    return std::unexpected(configuration_error(
        "Site '" + account.m_site +
        "' is not a site id; use lowercase letters, digits and '-', as in qoder or qoder-cn."));
  }

  std::string requested;
  if (requested_name.has_value()) {
    requested = std::string{strings::trim(*requested_name)};
  }
  if (requested.empty()) {
    requested = std::string{strings::trim(account.m_name)};
  }
  if (!requested.empty()) {
    if (auto const valid = validate_account_name(requested); !valid) {
      return std::unexpected(valid.error());
    }
  }

  auto const site = account.m_site;
  auto const identity = identity_fields{
      .m_uid = account.m_uid,
      .m_account_id = account.m_account_id,
      .m_email = account.m_email,
      .m_display_name = account.m_display_name,
  };
  auto const matched = find_by_identity(file, site, account.m_vpc, identity);
  if (!matched) {
    return std::unexpected(matched.error());
  }
  auto* const existing = *matched;

  if (existing != nullptr) {
    auto const existing_key = existing->key();
    auto const name = requested.empty() ? existing->m_name : requested;
    if (name != existing->m_name) {
      auto const* clash = find_by_name(file, site, name);
      if (clash != nullptr && clash->key() != existing_key) {
        return name_taken_error(name, site, clash->m_name);
      }
    }
    account.m_name = name;
    account.m_created_at =
        existing->m_created_at != 0 ? existing->m_created_at : account.m_created_at;
    auto const key = account.key();
    if (key == existing_key) {
      *existing = std::move(account);
      return existing;
    }
    auto const follows_default = is_default(file, site, existing_key);
    file.m_accounts.erase(existing_key);
    auto const inserted = file.m_accounts.insert_or_assign(key, std::move(account));
    if (follows_default) {
      file.m_defaults[site] = key;
    }
    return &inserted.first->second;
  }

  std::string name = requested;
  std::vector<std::string> existing_names;
  for (auto const& entry : file.m_accounts) {
    if (entry.second.m_site == site) {
      existing_names.push_back(entry.second.m_name);
    }
  }
  if (name.empty()) {
    name = unique_account_name(
        slug_account_name(account.m_email, account.m_display_name, account.m_uid), existing_names);
  }

  auto* const clash = find_by_name(file, site, name);
  bool follows_default = false;
  if (clash != nullptr) {
    if (!replace_existing_name) {
      return name_taken_error(name, site, clash->m_name);
    }
    account.m_created_at = clash->m_created_at != 0 ? clash->m_created_at : account.m_created_at;
    auto const clash_key = clash->key();
    follows_default = is_default(file, site, clash_key);
    file.m_accounts.erase(clash_key);
  }

  account.m_name = name;
  auto const key = account.key();
  auto const inserted = file.m_accounts.insert_or_assign(key, std::move(account));
  if (follows_default || existing_names.empty()) {
    // Only a site's very first account claims the default; a hand-edited file with accounts but no
    // `defaults` keeps whichever account was already acting as the default.
    file.m_defaults[site] = key;
  }
  return &inserted.first->second;
}

account_match match_accounts(credential_file const& file, std::string_view site,
                             std::string_view selector) {
  account_match result;
  auto const site_id = normalize_site_id(site);
  for (auto const& entry : file.m_accounts) {
    if (entry.second.m_site == site_id) {
      result.m_candidates.push_back(&entry.second);
    }
  }
  std::sort(result.m_candidates.begin(), result.m_candidates.end(),
            [](account_record const* left, account_record const* right) {
              auto const order = compare_ignore_case(left->m_name, right->m_name);
              return order != 0 ? order < 0 : left->key() < right->key();
            });
  result.m_site_has_accounts = !result.m_candidates.empty();

  auto const wanted = strings::trim(selector);
  if (wanted.empty()) {
    result.m_candidates.clear();
    return result;
  }

  auto const all = result.m_candidates;
  auto const exact = [&](auto const& field) {
    std::vector<account_record const*> matches;
    for (auto const* candidate : all) {
      if (strings::equals_ignore_case(field(*candidate), wanted)) {
        matches.push_back(candidate);
      }
    }
    return matches;
  };

  auto matches = exact([](account_record const& candidate) { return candidate.m_name; });
  if (matches.empty()) {
    matches = exact([](account_record const& candidate) { return candidate.m_uid; });
  }
  if (matches.empty()) {
    matches = exact([](account_record const& candidate) { return candidate.m_email; });
  }
  if (matches.empty()) {
    for (auto const* candidate : all) {
      if (prefix_match(candidate->m_name, wanted) || prefix_match(candidate->m_uid, wanted) ||
          prefix_match(candidate->m_email, wanted)) {
        matches.push_back(candidate);
      }
    }
  }
  result.m_candidates = std::move(matches);
  return result;
}

account_record const* default_account(credential_file const& file, std::string_view site) {
  auto const site_id = normalize_site_id(site);
  auto const found = file.m_defaults.find(site_id);
  if (found != file.m_defaults.end()) {
    auto const stored = file.m_accounts.find(found->second);
    if (stored != file.m_accounts.end() && stored->second.m_site == site_id) {
      return &stored->second;
    }
  }
  auto const accounts = accounts_for_site(file, site_id);
  if (accounts.size() == 1) {
    return accounts.front();
  }
  return nullptr;
}

result<void> set_default_account(credential_file& file, std::string_view key) {
  auto const account = file.m_accounts.find(key);
  if (account == file.m_accounts.end()) {
    return std::unexpected(
        configuration_error("No account is stored under '" + std::string{key} + "'."));
  }
  file.m_defaults[account->second.m_site] = std::string{key};
  return {};
}

result<removal_result> remove_accounts(credential_file& file, std::string_view site,
                                       std::span<std::string const> selectors, bool all) {
  auto const site_id = normalize_site_id(site);
  auto const site_accounts = accounts_for_site(file, site_id);

  std::vector<std::string> removals;
  if (all) {
    if (site_accounts.empty()) {
      return no_accounts_error(site_id);
    }
    for (auto const* account : site_accounts) {
      removals.push_back(account->key());
    }
  } else {
    if (selectors.empty()) {
      return std::unexpected(
          configuration_error("An account selector is required; use --all to remove every "
                              "account of site '" +
                              site_id + "'."));
    }
    std::set<std::string, std::less<>> selected;
    for (auto const& selector : selectors) {
      auto const match = match_accounts(file, site_id, selector);
      if (match.m_candidates.empty()) {
        if (!match.m_site_has_accounts) {
          return no_accounts_error(site_id);
        }
        return std::unexpected(configuration_error(
            "No account matches '" + std::string{strings::trim(selector)} + "' for site '" +
            site_id + "'; available accounts: " + join_names(site_accounts)));
      }
      if (match.m_candidates.size() > 1) {
        return std::unexpected(configuration_error(
            "'" + std::string{strings::trim(selector)} + "' matches several accounts for site '" +
            site_id + "': " + join_names(match.m_candidates) + "; use the exact account name"));
      }
      selected.insert(match.m_candidates.front()->key());
    }
    for (auto const* account : site_accounts) {
      if (selected.contains(account->key())) {
        removals.push_back(account->key());
      }
    }
  }

  for (auto const& key : removals) {
    file.m_accounts.erase(key);
  }

  auto const previous = [&] {
    auto const found = file.m_defaults.find(site_id);
    return found == file.m_defaults.end() ? std::string{} : found->second;
  }();
  file.m_defaults.erase(site_id);

  if (file.m_accounts.empty()) {
    file.m_defaults.clear();
    return removal_result{.m_removed_keys = std::move(removals), .m_file_empty = true};
  }

  auto const remaining = accounts_for_site(file, site_id);
  if (!remaining.empty()) {
    auto const keep =
        std::find_if(remaining.begin(), remaining.end(),
                     [&](account_record const* account) { return account->key() == previous; });
    file.m_defaults[site_id] = keep != remaining.end() ? (*keep)->key() : remaining.front()->key();
  }
  return removal_result{.m_removed_keys = std::move(removals), .m_file_empty = false};
}

} // namespace qoderz::config
