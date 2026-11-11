#include "common/error.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::testing::temp_dir;

namespace {

account_record full_account() {
  return account_record{
      .m_site = "qoder-cn",
      .m_name = "ada",
      .m_vpc = "acme",
      .m_access_token = "test-access-token-not-real",
      .m_refresh_token = "test-refresh-token-not-real",
      .m_expires_at = 1'760'000'000'123,
      .m_uid = "u-1",
      .m_display_name = "Ada Lovelace",
      .m_email = "ada@example.test",
      .m_account_id = "ada@example.test",
      .m_machine_id = "machine-42",
      .m_device_token = "test-device-token-not-real",
      .m_device_refresh_token = "test-device-refresh-not-real",
      .m_device_chat = true,
      .m_pat_auth = true,
      .m_pat = "pt-test-not-real",
      .m_created_at = 1'760'000'000'000,
  };
}

void write_text(std::filesystem::path const& path, std::string_view text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(std::filesystem::path const& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

#if !defined(_WIN32)
int mode_of(std::filesystem::path const& path) {
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  return static_cast<int>(info.st_mode & 0777);
}
#endif

} // namespace

TEST_CASE("a missing credential file loads as empty", "[config][credentials]") {
  temp_dir dir;
  credential_store const store{dir.file("credentials.json")};

  auto const loaded = store.load();
  REQUIRE(loaded.has_value());
  CHECK(loaded->m_version == 1);
  CHECK(loaded->m_accounts.empty());
  CHECK(loaded->m_defaults.empty());
}

TEST_CASE("save and load round trip every field", "[config][credentials]") {
  temp_dir dir;
  credential_store const store{dir.file("credentials.json")};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  file.m_defaults.emplace("qoder-cn", account.key());

  REQUIRE(store.save(file).has_value());
  auto const loaded = store.load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->m_accounts.size() == 1);

  auto const& restored = loaded->m_accounts.at("qoder-cn/ada");
  CHECK(restored.m_site == account.m_site);
  CHECK(restored.m_name == account.m_name);
  CHECK(restored.m_vpc == account.m_vpc);
  CHECK(restored.m_access_token == account.m_access_token);
  CHECK(restored.m_refresh_token == account.m_refresh_token);
  CHECK(restored.m_expires_at == account.m_expires_at);
  CHECK(restored.m_uid == account.m_uid);
  CHECK(restored.m_display_name == account.m_display_name);
  CHECK(restored.m_email == account.m_email);
  CHECK(restored.m_account_id == account.m_account_id);
  CHECK(restored.m_machine_id == account.m_machine_id);
  CHECK(restored.m_device_token == account.m_device_token);
  CHECK(restored.m_device_refresh_token == account.m_device_refresh_token);
  CHECK(restored.m_device_chat == account.m_device_chat);
  CHECK(restored.m_pat_auth == account.m_pat_auth);
  CHECK(restored.m_pat == account.m_pat);
  CHECK(restored.m_created_at == account.m_created_at);
  CHECK(loaded->m_defaults.at("qoder-cn") == "qoder-cn/ada");
}

TEST_CASE("empty VPC and PAT survive the round trip", "[config][credentials]") {
  temp_dir dir;
  credential_store const store{dir.file("credentials.json")};

  auto account = full_account();
  account.m_vpc.clear();
  account.m_pat.clear();
  account.m_pat_auth = false;
  credential_file file;
  file.m_accounts.emplace(account.key(), account);

  REQUIRE(store.save(file).has_value());
  auto const loaded = store.load();
  REQUIRE(loaded.has_value());
  auto const& restored = loaded->m_accounts.at("qoder-cn/ada");
  CHECK(restored.m_vpc.empty());
  CHECK(restored.m_pat.empty());
  CHECK_FALSE(restored.m_pat_auth);
}

TEST_CASE("the credential file itself is private", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

#if !defined(_WIN32)
  CHECK(mode_of(path) == 0600);
#endif
}

#if !defined(_WIN32)
TEST_CASE("the directory holding the credential file is not tightened", "[config][credentials]") {
  temp_dir dir;
  auto const shared = dir.file("shared");
  REQUIRE(std::filesystem::create_directories(shared));
  REQUIRE(::chmod(shared.c_str(), 0755) == 0);
  REQUIRE(mode_of(shared) == 0755);

  auto const path = shared / "credentials.json";
  credential_store const store{path};
  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

  CHECK(mode_of(path) == 0600);
  CHECK(mode_of(shared) == 0755);
}
#endif

TEST_CASE("an oversized file is rejected", "[config][credentials]") {
  temp_dir dir;

  SECTION("exactly 16 MiB is accepted") {
    auto const path = dir.file("exact.json");
    write_text(path, "{}");
    std::error_code error;
    std::filesystem::resize_file(path, 16U * 1024U * 1024U, error);
    REQUIRE_FALSE(error);

    auto const loaded = credential_store{path}.load();
    // The file is padded with NUL bytes, so it is not valid JSON; what matters is that the size
    // check let it through instead of reporting the 16 MiB limit.
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().m_message.find("16 MiB") == std::string::npos);
  }

  SECTION("16 MiB plus one byte is rejected") {
    auto const path = dir.file("over.json");
    write_text(path, "{}");
    std::error_code error;
    std::filesystem::resize_file(path, 16U * 1024U * 1024U + 1U, error);
    REQUIRE_FALSE(error);

    auto const loaded = credential_store{path}.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().m_message.find("16 MiB") != std::string::npos);
  }
}

TEST_CASE("the file is written as version 1 snake_case JSON", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

  auto const document = nlohmann::json::parse(read_text(path));
  CHECK(document.at("version") == 1);
  auto const& entry = document.at("accounts").at("qoder-cn/ada");
  CHECK(entry.at("site") == "qoder-cn");
  CHECK(entry.at("name") == "ada");
  CHECK(entry.at("vpc") == "acme");
  CHECK(entry.at("access_token") == "test-access-token-not-real");
  CHECK(entry.at("device_chat") == true);
  CHECK(entry.at("created_at") == 1'760'000'000'000);
  CHECK(entry.contains("accessToken") == false);
  CHECK(entry.contains("AccessToken") == false);
}

TEST_CASE("a malformed file is a configuration error", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(path, "{ not json");

  credential_store const store{path};
  auto const loaded = store.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_kind == qoderz::error_kind::configuration);
  CHECK(loaded.error().m_message.find(path.string()) != std::string::npos);
  CHECK(loaded.error().m_message.find("JSON") != std::string::npos);
  CHECK(read_text(path) == "{ not json");
}

TEST_CASE("an unsupported version is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  write_text(path, R"({"version": 2, "accounts": {}})");
  auto const newer = store.load();
  REQUIRE_FALSE(newer.has_value());
  CHECK(newer.error().m_message.find("version 2") != std::string::npos);

  write_text(path, R"({"accounts": {}})");
  auto const missing = store.load();
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().m_message.find("no version") != std::string::npos);

  write_text(path, R"({"version": "1", "accounts": {}})");
  CHECK_FALSE(store.load().has_value());
}

TEST_CASE("an account whose key disagrees with its fields is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(path,
             R"({"version": 1, "accounts": {"qoder-cn/ada": {"site": "qoder", "name": "ada"}}})");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("does not match") != std::string::npos);
}

TEST_CASE("duplicate account names within a site ignores case", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(path, R"({
    "version": 1,
    "accounts": {
      "qoder-cn/ada": {"site": "qoder-cn", "name": "ada"},
      "qoder-cn/ADA": {"site": "qoder-cn", "name": "ADA"}
    }
  })");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("more than one account named") != std::string::npos);
}
TEST_CASE("a stored site id must look like a site id", "[config][credentials]") {
  temp_dir dir;

  SECTION("a slash in the site is rejected") {
    auto const path = dir.file("slash.json");
    write_text(
        path,
        R"({"version": 1, "accounts": {"qoder/cn/ada": {"site": "qoder/cn", "name": "ada"}}})");
    auto const loaded = credential_store{path}.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().m_message.find("site id") != std::string::npos);
  }

  SECTION("an uppercase site is rejected instead of silently kept") {
    auto const path = dir.file("upper.json");
    write_text(path, R"({"version": 1, "accounts": {"QODER-CN/ada": {"site": "QODER-CN",
                      "name": "ada"}}})");
    auto const loaded = credential_store{path}.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().m_message.find("site id") != std::string::npos);
  }

  SECTION("a canonical site id is accepted") {
    auto const path = dir.file("ok.json");
    write_text(
        path,
        R"({"version": 1, "accounts": {"qoder-cn/ada": {"site": "qoder-cn", "name": "ada"}}})");
    REQUIRE(credential_store{path}.load().has_value());
  }
}

TEST_CASE("a dangling default is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(path, R"({
    "version": 1,
    "accounts": {"qoder-cn/ada": {"site": "qoder-cn", "name": "ada"}},
    "defaults": {"qoder-cn": "qoder-cn/ghost"}
  })");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("does not exist") != std::string::npos);
}

TEST_CASE("a default pointing at another site is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(path, R"({
    "version": 1,
    "accounts": {"qoder/ada": {"site": "qoder", "name": "ada"}},
    "defaults": {"qoder-cn": "qoder/ada"}
  })");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("of site") != std::string::npos);
}

TEST_CASE("a field of the wrong JSON type is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(
      path,
      R"({"version": 1, "accounts": {"qoder-cn/ada": {"site": "qoder-cn", "name": "ada", "expires_at": "soon"}}})");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("expires_at") != std::string::npos);
}

TEST_CASE("an invalid account name is rejected", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  write_text(
      path,
      R"({"version": 1, "accounts": {"qoder-cn/ada lovelace": {"site": "qoder-cn", "name": "ada lovelace"}}})");

  auto const loaded = credential_store{path}.load();
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().m_message.find("invalid name") != std::string::npos);
}

TEST_CASE("save refuses to overwrite a file it cannot read", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  constexpr std::string_view broken = "{ not json";
  write_text(path, broken);

  credential_store const store{path};
  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);

  auto const saved = store.save(file);
  REQUIRE_FALSE(saved.has_value());
  CHECK(saved.error().m_kind == qoderz::error_kind::configuration);
  CHECK(read_text(path) == broken);
}

TEST_CASE("save leaves no temporary file behind", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

  std::size_t entries = 0;
  for (auto const& entry : std::filesystem::directory_iterator{dir.path()}) {
    (void)entry;
    ++entries;
  }
  CHECK(entries == 1);
  CHECK(std::filesystem::exists(path));
}

TEST_CASE("an invalid in-memory file is never written", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  account_record account;
  account.m_site = "qoder-cn";
  account.m_name = "ada";
  file.m_accounts.emplace("qoder-cn/grace", account);

  auto const saved = store.save(file);
  REQUIRE_FALSE(saved.has_value());
  CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("remove_file is idempotent", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

  REQUIRE(store.remove_file().has_value());
  CHECK_FALSE(std::filesystem::exists(path));
  CHECK(store.remove_file().has_value());
  CHECK(store.load()->m_accounts.empty());
}

TEST_CASE("save replaces an existing credential file", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

  file.m_accounts.at("qoder-cn/ada").m_email = "grace@example.test";
  REQUIRE(store.save(file).has_value());

  auto const loaded = store.load();
  REQUIRE(loaded.has_value());
  CHECK(loaded->m_accounts.at("qoder-cn/ada").m_email == "grace@example.test");
}

TEST_CASE("a failed save leaves no temporary file behind", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("credentials.json");
  constexpr std::string_view broken = "{ not json";
  write_text(path, broken);

  credential_store const store{path};
  credential_file readable;
  auto const account = full_account();
  readable.m_accounts.emplace(account.key(), account);
  REQUIRE_FALSE(store.save(readable).has_value());

  credential_file inconsistent;
  account_record mismatched;
  mismatched.m_site = "qoder-cn";
  mismatched.m_name = "ada";
  inconsistent.m_accounts.emplace("qoder-cn/grace", mismatched);
  REQUIRE_FALSE(store.save(inconsistent).has_value());

  std::size_t entries = 0;
  for (auto const& entry : std::filesystem::directory_iterator{dir.path()}) {
    INFO("entry: " << entry.path().filename().string());
    CHECK_FALSE(entry.path().filename().string().ends_with(".tmp"));
    ++entries;
  }
  CHECK(entries == 1);
  CHECK(read_text(path) == broken);
}

TEST_CASE("save creates a missing private directory", "[config][credentials]") {
  temp_dir dir;
  auto const path = dir.file("nested").append("qoderz").append("credentials.json");
  credential_store const store{path};

  credential_file file;
  auto const account = full_account();
  file.m_accounts.emplace(account.key(), account);
  REQUIRE(store.save(file).has_value());

#if !defined(_WIN32)
  CHECK(mode_of(path.parent_path()) == 0700);
#endif
  auto const loaded = store.load();
  REQUIRE(loaded.has_value());
  CHECK(loaded->m_accounts.contains("qoder-cn/ada"));
}
