#pragma once

#include "common/error.hpp"
#include "config/credentials.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::config {

inline constexpr std::size_t max_account_name_length = 64;

/// `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`: 1–64 characters, starting with a letter or digit.
[[nodiscard]] constexpr bool is_valid_account_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > max_account_name_length) {
    return false;
  }
  auto const is_alnum = [](char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
  };
  if (!is_alnum(name.front())) {
    return false;
  }
  for (auto const value : name) {
    if (!is_alnum(value) && value != '.' && value != '_' && value != '-') {
      return false;
    }
  }
  return true;
}

/// Trims the name and validates it; the error names the allowed characters.
[[nodiscard]] result<void> validate_account_name(std::string_view name);

/// The first non-empty slug of the email local part, the display name and the uid; `"account"`
/// when all three are empty.
[[nodiscard]] std::string slug_account_name(std::string_view email, std::string_view display_name,
                                            std::string_view uid);

/// `base` unless a stored name already uses it, otherwise `base-2`, `base-3`, …; comparison ignores
/// ASCII case and every result fits `max_account_name_length`.
[[nodiscard]] std::string unique_account_name(std::string_view base,
                                              std::span<std::string const> existing_names);

/// The four fields one sign-in can identify a person by, in priority order.
struct identity_fields {
  std::string_view m_uid{};
  std::string_view m_account_id{};
  std::string_view m_email{};
  std::string_view m_display_name{};
};

/// Accounts of one site, ordered by name ignoring ASCII case. The pointers stay valid until the
/// file's `m_accounts` is mutated.
[[nodiscard]] std::vector<account_record const*> accounts_for_site(credential_file const& file,
                                                                   std::string_view site);

/// The account of `site` whose identity matches `identity`, or nullptr. Matching compares the VPC
/// instance first (an empty VPC only matches an empty stored VPC), then takes the first non-empty
/// of uid, account_id, email and display_name and compares it, trimmed, with the same stored field.
[[nodiscard]] result<account_record*> find_by_identity(credential_file& file, std::string_view site,
                                                       std::string_view vpc,
                                                       identity_fields const& identity);

/// Inserts `account`, or updates the account with the same identity in place. A non-empty
/// `requested_name` — or a non-empty `account.m_name` when that is empty — names the account and
/// renames an existing one, moving the site default with it; a name already used by another
/// identity is a configuration error unless `replace_existing_name` is set, in which case that
/// account is replaced. Without either name a unique slug is generated. The first account of a site
/// becomes its default.
[[nodiscard]] result<account_record*> upsert_account(credential_file& file, account_record account,
                                                     std::optional<std::string> requested_name,
                                                     bool replace_existing_name);

struct account_match {
  std::vector<account_record const*> m_candidates;
  bool m_site_has_accounts = false;
};

/// Selector matching for `--account`: exact name, then uid, then email (all ignoring ASCII case),
/// then a unique prefix of any of them. Several candidates means the selector was ambiguous; an
/// empty candidate list with `m_site_has_accounts` false means the site has no accounts at all.
[[nodiscard]] account_match match_accounts(credential_file const& file, std::string_view site,
                                           std::string_view selector);

/// The stored default when it still exists, otherwise the site's only account, otherwise nullptr.
[[nodiscard]] account_record const* default_account(credential_file const& file,
                                                    std::string_view site);

/// Points the default of the account's site at `key`; an unknown key is a configuration error.
[[nodiscard]] result<void> set_default_account(credential_file& file, std::string_view key);

struct removal_result {
  std::vector<std::string> m_removed_keys;
  bool m_file_empty = false;
};

/// Removes `all` accounts of the site or one account per selector. A selector with no match or an
/// ambiguous match is a configuration error. The default moves to the first remaining account by
/// name; when the file becomes empty every default is dropped and `m_file_empty` is set.
[[nodiscard]] result<removal_result> remove_accounts(credential_file& file, std::string_view site,
                                                     std::span<std::string const> selectors,
                                                     bool all);

} // namespace qoderz::config
