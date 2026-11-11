#include "common/error.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using qoderz::config::account_match;
using qoderz::config::account_record;
using qoderz::config::accounts_for_site;
using qoderz::config::credential_file;
using qoderz::config::default_account;
using qoderz::config::find_by_identity;
using qoderz::config::identity_fields;
using qoderz::config::is_valid_account_name;
using qoderz::config::match_accounts;
using qoderz::config::remove_accounts;
using qoderz::config::set_default_account;
using qoderz::config::slug_account_name;
using qoderz::config::unique_account_name;
using qoderz::config::upsert_account;
using qoderz::config::validate_account_name;

namespace {

account_record account_of(std::string site, std::string name, std::string uid,
                          std::string email = {}, std::string vpc = {},
                          std::string display_name = {}) {
  account_record account;
  account.m_site = std::move(site);
  account.m_name = std::move(name);
  account.m_uid = std::move(uid);
  account.m_email = std::move(email);
  account.m_vpc = std::move(vpc);
  account.m_display_name = std::move(display_name);
  account.m_access_token = "test-access-token-not-real";
  account.m_refresh_token = "test-refresh-token-not-real";
  account.m_expires_at = 1'760'000'000'000;
  account.m_machine_id = "machine-42";
  account.m_created_at = 1'760'000'000'000;
  return account;
}

std::vector<std::string> names_of(credential_file const& file, std::string_view site) {
  std::vector<std::string> names;
  for (auto const* account : accounts_for_site(file, site)) {
    names.push_back(account->m_name);
  }
  return names;
}

account_record* insert(credential_file& file, account_record account) {
  auto const key = account.key();
  auto const inserted = file.m_accounts.emplace(key, std::move(account));
  return &inserted.first->second;
}

} // namespace

TEST_CASE("valid account names are accepted", "[config][accounts]") {
  std::vector<std::string> const names{"ada", "Ada.Lovelace", "u_1",
                                       "x-y", "9lives",       std::string(64, 'a')};
  for (auto const& name : names) {
    INFO("name: " << name);
    CHECK(is_valid_account_name(name));
    CHECK(validate_account_name(name).has_value());
  }
}

TEST_CASE("invalid account names are rejected with the allowed characters", "[config][accounts]") {
  std::vector<std::string> const names{"",
                                       "-ada",
                                       ".ada",
                                       "_ada",
                                       "ada lovelace",
                                       "ada/lovelace",
                                       "ada@example.com",
                                       "ada!",
                                       std::string(65, 'a')};
  for (auto const& name : names) {
    INFO("name: " << name);
    CHECK_FALSE(is_valid_account_name(name));
    auto const valid = validate_account_name(name);
    REQUIRE_FALSE(valid.has_value());
    CHECK(valid.error().m_kind == qoderz::error_kind::configuration);
    CHECK(valid.error().m_message.find("A-Z") != std::string::npos);
  }
}

TEST_CASE("account names are trimmed before validation", "[config][accounts]") {
  CHECK(validate_account_name("  ada ").has_value());
  CHECK_FALSE(validate_account_name("   ").has_value());
}

TEST_CASE("slugs prefer the email local part", "[config][accounts]") {
  CHECK(slug_account_name("Ada.Lovelace+test@example.com", "Ada Lovelace", "u1") ==
        "ada-lovelace-test");
  CHECK(slug_account_name("ada@example.test", "", "") == "ada");
  CHECK(slug_account_name("", "Ada Lovelace", "u1") == "ada-lovelace");
  CHECK(slug_account_name("", "", "u1") == "u1");
  CHECK(slug_account_name("", "***", "!!!") == "account");
  CHECK(slug_account_name("", "", "") == "account");
  CHECK(slug_account_name("--ada--", "", "") == "ada");
}

TEST_CASE("slugs are truncated to the maximum name length", "[config][accounts]") {
  auto const slug = slug_account_name(std::string(90, 'a') + "@example.test", "", "");
  CHECK(slug == std::string(64, 'a'));
  CHECK(is_valid_account_name(slug));
}

TEST_CASE("unique names append numeric suffixes", "[config][accounts]") {
  std::vector<std::string> const none{};
  std::vector<std::string> const one{"ada"};
  std::vector<std::string> const two{"ada", "ada-2"};
  std::vector<std::string> const other_case{"ADA"};

  CHECK(unique_account_name("ada", none) == "ada");
  CHECK(unique_account_name("ada", one) == "ada-2");
  CHECK(unique_account_name("ada", two) == "ada-3");
  CHECK(unique_account_name("ada", other_case) == "ada-2");
}

TEST_CASE("unique suffixes keep names inside the length limit", "[config][accounts]") {
  auto const base = std::string(64, 'a');
  std::vector<std::string> const taken{base};

  auto const unique = unique_account_name(base, taken);
  CHECK(unique == std::string(62, 'a') + "-2");
  CHECK(is_valid_account_name(unique));
}

TEST_CASE("the first account of a site becomes the default", "[config][accounts]") {
  credential_file file;
  auto const first = upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"),
                                    std::nullopt, false);
  REQUIRE(first.has_value());
  CHECK((*first)->m_name == "ada");
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/ada");

  auto const second = upsert_account(file, account_of("qoder-cn", "", "u2", "bob@example.test"),
                                     std::nullopt, false);
  REQUIRE(second.has_value());
  CHECK((*second)->m_name == "bob");
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/ada");
}

TEST_CASE("the same identity updates in place and keeps its name and created_at",
          "[config][accounts]") {
  credential_file file;
  auto stored = account_of("qoder-cn", "", "u1", "ada@example.test");
  stored.m_created_at = 1'700'000'000'000;
  auto const first = upsert_account(file, stored, std::nullopt, false);
  REQUIRE(first.has_value());

  auto rotated = stored;
  rotated.m_access_token = "test-rotated-not-real";
  rotated.m_created_at = 1'800'000'000'000;
  auto const second = upsert_account(file, rotated, std::nullopt, false);
  REQUIRE(second.has_value());

  CHECK(file.m_accounts.size() == 1);
  CHECK((*second)->m_name == "ada");
  CHECK((*second)->m_access_token == "test-rotated-not-real");
  CHECK((*second)->m_created_at == 1'700'000'000'000);
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/ada");
}

TEST_CASE("an explicit new name renames the account and follows the default",
          "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"), std::nullopt,
                         false)
              .has_value());

  auto const renamed = upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"),
                                      std::string{"grace"}, false);
  REQUIRE(renamed.has_value());
  CHECK((*renamed)->m_name == "grace");
  CHECK(file.m_accounts.contains("qoder-cn/grace"));
  CHECK_FALSE(file.m_accounts.contains("qoder-cn/ada"));
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/grace");
}

TEST_CASE("a requested name owned by another identity is rejected", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"), std::nullopt,
                         false)
              .has_value());

  auto const clash = upsert_account(file, account_of("qoder-cn", "", "u2", "bob@example.test"),
                                    std::string{"ADA"}, false);
  REQUIRE_FALSE(clash.has_value());
  CHECK(clash.error().m_kind == qoderz::error_kind::configuration);
  CHECK(clash.error().m_message.find("already exists") != std::string::npos);
  CHECK(file.m_accounts.size() == 1);
}

TEST_CASE("replacing an existing name frees its slot", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "work", "u1", "ada@example.test"),
                         std::nullopt, false)
              .has_value());

  auto const replaced = upsert_account(file, account_of("qoder-cn", "", "u2", "bob@example.test"),
                                       std::string{"work"}, true);
  REQUIRE(replaced.has_value());
  CHECK((*replaced)->m_name == "work");
  CHECK((*replaced)->m_uid == "u2");
  CHECK(file.m_accounts.size() == 1);
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/work");
}

TEST_CASE("generated names suffix collisions", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"), std::nullopt,
                         false)
              .has_value());
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u2", "ada@example.test"), std::nullopt,
                         false)
              .has_value());
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u3", "ada@example.test"), std::nullopt,
                         false)
              .has_value());

  CHECK(names_of(file, "qoder-cn") == std::vector<std::string>{"ada", "ada-2", "ada-3"});
}

TEST_CASE("identity matching prefers uid then account id then email then display name",
          "[config][accounts]") {
  credential_file file;
  auto stored = account_of("qoder-cn", "ada", "u1", "ada@example.test");
  stored.m_account_id = "account-1";
  insert(file, stored);

  auto const by_uid = find_by_identity(file, "qoder-cn", "",
                                       identity_fields{.m_uid = "u1", .m_email = "other@x.test"});
  REQUIRE(by_uid.has_value());
  REQUIRE(*by_uid != nullptr);
  CHECK((*by_uid)->m_name == "ada");

  auto const by_account_id =
      find_by_identity(file, "qoder-cn", "", identity_fields{.m_account_id = "account-1"});
  REQUIRE(by_account_id.has_value());
  REQUIRE(*by_account_id != nullptr);
  CHECK((*by_account_id)->m_name == "ada");

  auto const by_email =
      find_by_identity(file, "qoder-cn", "", identity_fields{.m_email = "ada@example.test"});
  REQUIRE(by_email.has_value());
  REQUIRE(*by_email != nullptr);
  CHECK((*by_email)->m_name == "ada");

  auto const by_display =
      find_by_identity(file, "qoder-cn", "", identity_fields{.m_display_name = "Ada Lovelace"});
  REQUIRE(by_display.has_value());
  CHECK(*by_display == nullptr);

  auto const nothing = find_by_identity(file, "qoder-cn", "", identity_fields{});
  REQUIRE(nothing.has_value());
  CHECK(*nothing == nullptr);
}

TEST_CASE("identity matching separates VPC instances", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test"), std::nullopt,
                         false)
              .has_value());
  REQUIRE(upsert_account(file, account_of("qoder-cn", "", "u1", "ada@example.test", "acme"),
                         std::nullopt, false)
              .has_value());

  CHECK(names_of(file, "qoder-cn") == std::vector<std::string>{"ada", "ada-2"});

  auto const plain = find_by_identity(file, "qoder-cn", "", identity_fields{.m_uid = "u1"});
  REQUIRE(plain.has_value());
  REQUIRE(*plain != nullptr);
  CHECK((*plain)->m_vpc.empty());

  auto const vpc = find_by_identity(file, "qoder-cn", "acme", identity_fields{.m_uid = "u1"});
  REQUIRE(vpc.has_value());
  REQUIRE(*vpc != nullptr);
  CHECK((*vpc)->m_vpc == "acme");
}

TEST_CASE("upsert trims identity fields and the site", "[config][accounts]") {
  credential_file file;
  auto stored = account_of(" qoder-cn ", "", " u1 ", " ada@example.test ");
  REQUIRE(upsert_account(file, stored, std::nullopt, false).has_value());

  auto const found = find_by_identity(file, "qoder-cn", "", identity_fields{.m_uid = "u1"});
  REQUIRE(found.has_value());
  REQUIRE(*found != nullptr);
  CHECK((*found)->m_site == "qoder-cn");
  CHECK((*found)->m_uid == "u1");
  CHECK((*found)->m_email == "ada@example.test");
}

TEST_CASE("upsert rejects an empty site and a slash in the site", "[config][accounts]") {
  credential_file file;
  auto const empty = upsert_account(file, account_of("", "", "u1"), std::nullopt, false);
  REQUIRE_FALSE(empty.has_value());

  auto const slash = upsert_account(file, account_of("qoder/cn", "", "u1"), std::nullopt, false);
  REQUIRE_FALSE(slash.has_value());

  auto const underscore =
      upsert_account(file, account_of("qoder_cn", "", "u1"), std::nullopt, false);
  REQUIRE_FALSE(underscore.has_value());
  CHECK(underscore.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("a mixed-case site is stored under its canonical id", "[config][accounts]") {
  credential_file file;
  auto const stored = upsert_account(file, account_of("QODER-CN", "", "u1", "ada@example.test"),
                                     std::nullopt, false);
  REQUIRE(stored.has_value());
  CHECK((*stored)->m_site == "qoder-cn");
  CHECK(file.m_accounts.contains("qoder-cn/ada"));
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/ada");

  CHECK(accounts_for_site(file, " Qoder-Cn ").size() == 1);
  REQUIRE(default_account(file, "QODER-CN") != nullptr);
  CHECK(default_account(file, "QODER-CN")->m_name == "ada");
  CHECK(match_accounts(file, "Qoder-CN", "ada").m_candidates.size() == 1);
}

TEST_CASE("a second account does not claim the default from a hand-edited file",
          "[config][accounts]") {
  credential_file file;
  insert(file, account_of("qoder-cn", "ada", "u1", "ada@example.test"));
  REQUIRE_FALSE(file.m_defaults.contains("qoder-cn"));
  REQUIRE(default_account(file, "qoder-cn") != nullptr);

  REQUIRE(upsert_account(file, account_of("qoder-cn", "bob", "u2", "bob@example.test"),
                         std::nullopt, false)
              .has_value());

  // The site now holds two accounts and no `defaults` entry, so nothing acts as the default; the
  // point is that the new account did not claim it by writing `defaults["qoder-cn"]`.
  CHECK_FALSE(file.m_defaults.contains("qoder-cn"));
  CHECK(default_account(file, "qoder-cn") == nullptr);
  CHECK(accounts_for_site(file, "qoder-cn").size() == 2);
}

TEST_CASE("match_accounts resolves names, uids and emails", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "ada", "u1", "ada@example.test"),
                         std::nullopt, false)
              .has_value());
  REQUIRE(upsert_account(file, account_of("qoder-cn", "bob", "u2", "bob@example.test"),
                         std::nullopt, false)
              .has_value());

  auto const by_name = match_accounts(file, "qoder-cn", "BOB");
  REQUIRE(by_name.m_candidates.size() == 1);
  CHECK(by_name.m_candidates.front()->m_name == "bob");

  auto const by_uid = match_accounts(file, "qoder-cn", "u1");
  REQUIRE(by_uid.m_candidates.size() == 1);
  CHECK(by_uid.m_candidates.front()->m_name == "ada");

  auto const by_email = match_accounts(file, "qoder-cn", "bob@example.test");
  REQUIRE(by_email.m_candidates.size() == 1);
  CHECK(by_email.m_candidates.front()->m_name == "bob");

  auto const by_prefix = match_accounts(file, "qoder-cn", "ad");
  REQUIRE(by_prefix.m_candidates.size() == 1);
  CHECK(by_prefix.m_candidates.front()->m_name == "ada");
}

TEST_CASE("match_accounts reports ambiguity and missing accounts", "[config][accounts]") {
  credential_file file;
  REQUIRE(upsert_account(file, account_of("qoder-cn", "ada", "u1", "ada@example.test"),
                         std::nullopt, false)
              .has_value());
  REQUIRE(upsert_account(file, account_of("qoder-cn", "ada-2", "u2", "ada2@example.test"),
                         std::nullopt, false)
              .has_value());

  auto const ambiguous = match_accounts(file, "qoder-cn", "ad");
  CHECK(ambiguous.m_candidates.size() == 2);
  CHECK(ambiguous.m_site_has_accounts);

  auto const missing = match_accounts(file, "qoder-cn", "zoe");
  CHECK(missing.m_candidates.empty());
  CHECK(missing.m_site_has_accounts);

  auto const blank = match_accounts(file, "qoder-cn", "   ");
  CHECK(blank.m_candidates.empty());

  auto const other_site = match_accounts(file, "qoder", "ada");
  CHECK(other_site.m_candidates.empty());
  CHECK_FALSE(other_site.m_site_has_accounts);
}

TEST_CASE("the default account falls back to a single account", "[config][accounts]") {
  credential_file file;
  CHECK(default_account(file, "qoder-cn") == nullptr);

  insert(file, account_of("qoder-cn", "ada", "u1"));
  CHECK(default_account(file, "qoder-cn") != nullptr);
  CHECK(default_account(file, "qoder-cn")->m_name == "ada");

  insert(file, account_of("qoder-cn", "bob", "u2"));
  CHECK(default_account(file, "qoder-cn") == nullptr);

  file.m_defaults["qoder-cn"] = "qoder-cn/bob";
  REQUIRE(default_account(file, "qoder-cn") != nullptr);
  CHECK(default_account(file, "qoder-cn")->m_name == "bob");

  file.m_defaults["qoder-cn"] = "qoder-cn/ghost";
  CHECK(default_account(file, "qoder-cn") == nullptr);
}

TEST_CASE("set_default_account points the site default at a stored account", "[config][accounts]") {
  credential_file file;
  insert(file, account_of("qoder-cn", "ada", "u1"));
  insert(file, account_of("qoder-cn", "bob", "u2"));

  REQUIRE(set_default_account(file, "qoder-cn/bob").has_value());
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/bob");

  auto const missing = set_default_account(file, "qoder-cn/ghost");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("removing the default promotes the first remaining account by name",
          "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "bob", "u2"), std::nullopt, false).has_value());
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "zoe", "u3"), std::nullopt, false).has_value());

  std::array<std::string, 1> const selector{"ada"};
  auto const removed = remove_accounts(file, "qoder-cn", selector, false);
  REQUIRE(removed.has_value());
  CHECK(removed->m_removed_keys == std::vector<std::string>{"qoder-cn/ada"});
  CHECK_FALSE(removed->m_file_empty);
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/bob");
  CHECK(names_of(file, "qoder-cn") == std::vector<std::string>{"bob", "zoe"});
}

TEST_CASE("removing another account keeps the default", "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "bob", "u2"), std::nullopt, false).has_value());

  std::array<std::string, 1> const selector{"u2"};
  auto const removed = remove_accounts(file, "qoder-cn", selector, false);
  REQUIRE(removed.has_value());
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/ada");
}

TEST_CASE("removing by an ambiguous prefix is rejected", "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada-2", "u2"), std::nullopt, false).has_value());

  std::array<std::string, 1> const selector{"ad"};
  auto const removed = remove_accounts(file, "qoder-cn", selector, false);
  REQUIRE_FALSE(removed.has_value());
  CHECK(removed.error().m_message.find("several accounts") != std::string::npos);
  CHECK(file.m_accounts.size() == 2);
}

TEST_CASE("removing an unknown selector or no selector is rejected", "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());

  std::array<std::string, 1> const unknown{"zoe"};
  auto const missing = remove_accounts(file, "qoder-cn", unknown, false);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().m_message.find("No account matches") != std::string::npos);
  CHECK(missing.error().m_message.find("ada") != std::string::npos);

  std::span<std::string const> const none;
  auto const blank = remove_accounts(file, "qoder-cn", none, false);
  REQUIRE_FALSE(blank.has_value());
}
TEST_CASE("remove all clears only the selected site", "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "bob", "u2"), std::nullopt, false).has_value());
  REQUIRE(upsert_account(file, account_of("qoder", "ada", "u3"), std::nullopt, false).has_value());

  std::span<std::string const> const none;
  auto const removed = remove_accounts(file, "qoder-cn", none, true);
  REQUIRE(removed.has_value());
  CHECK(removed->m_removed_keys == std::vector<std::string>{"qoder-cn/ada", "qoder-cn/bob"});
  CHECK_FALSE(removed->m_file_empty);
  CHECK(names_of(file, "qoder-cn").empty());
  CHECK_FALSE(file.m_defaults.contains("qoder-cn"));
  CHECK(names_of(file, "qoder") == std::vector<std::string>{"ada"});
  CHECK(file.m_defaults.at("qoder") == "qoder/ada");
}

TEST_CASE("removing the last account empties the file and its defaults", "[config][accounts]") {
  credential_file file;
  REQUIRE(
      upsert_account(file, account_of("qoder-cn", "ada", "u1"), std::nullopt, false).has_value());

  std::span<std::string const> const none;
  auto const removed = remove_accounts(file, "qoder-cn", none, true);
  REQUIRE(removed.has_value());
  CHECK(removed->m_file_empty);
  CHECK(file.m_accounts.empty());
  CHECK(file.m_defaults.empty());
}

TEST_CASE("a pre-filled account name is used as the requested name", "[config][accounts]") {
  credential_file file;
  auto const stored = upsert_account(file, account_of("qoder-cn", "work", "u1", "ada@example.test"),
                                     std::nullopt, false);
  REQUIRE(stored.has_value());
  CHECK((*stored)->m_name == "work");
  CHECK(file.m_accounts.contains("qoder-cn/work"));
  CHECK(file.m_defaults.at("qoder-cn") == "qoder-cn/work");

  auto const refused =
      upsert_account(file, account_of("qoder-cn", "ada lovelace", "u2"), std::nullopt, false);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("identity matching lets the first non-empty field decide", "[config][accounts]") {
  credential_file file;
  insert(file, account_of("qoder-cn", "ada", "u1", "ada@example.test"));

  auto const conflicting = find_by_identity(
      file, "qoder-cn", "", identity_fields{.m_uid = "u9", .m_email = "ada@example.test"});
  REQUIRE(conflicting.has_value());
  CHECK(*conflicting == nullptr);

  auto const padded = find_by_identity(file, "qoder-cn", "", identity_fields{.m_uid = "  u1  "});
  REQUIRE(padded.has_value());
  REQUIRE(*padded != nullptr);
  CHECK((*padded)->m_name == "ada");
}
