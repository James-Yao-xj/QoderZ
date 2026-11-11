#include "common/error.hpp"
#include "config/paths.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using qoderz::config::default_credentials_path;
using qoderz::config::ensure_private_directory;
using qoderz::config::require_directory;
using qoderz::config::resolve_credentials_path;
using qoderz::testing::temp_dir;

namespace {

std::optional<std::string> text(std::string_view value) {
  return std::string{value};
}

#if !defined(_WIN32)
int mode_of(std::filesystem::path const& path) {
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  return static_cast<int>(info.st_mode & 0777);
}
#endif

} // namespace

TEST_CASE("the default credential path prefers an absolute XDG location", "[config][paths]") {
  auto const path = default_credentials_path(text("/xdg"), text("/home/ada"), text("/appdata"));
  REQUIRE(path.has_value());
  CHECK(*path == std::filesystem::path{"/xdg/qoderz/credentials.json"});
}

TEST_CASE("a relative XDG location is ignored", "[config][paths]") {
  auto const path = default_credentials_path(text("relative"), text("/home/ada"), text("/app"));
  REQUIRE(path.has_value());
  CHECK(*path == std::filesystem::path{"/home/ada/.config/qoderz/credentials.json"});
}

TEST_CASE("the default credential path falls back to HOME then APPDATA", "[config][paths]") {
  auto const home = default_credentials_path(std::nullopt, text("/home/ada"), text("/app"));
  REQUIRE(home.has_value());
  CHECK(*home == std::filesystem::path{"/home/ada/.config/qoderz/credentials.json"});

  auto const app_data = default_credentials_path(std::nullopt, std::nullopt, text("/app"));
  REQUIRE(app_data.has_value());
  CHECK(*app_data == std::filesystem::path{"/app/qoderz/credentials.json"});
}

TEST_CASE("the default credential path fails when no location exists", "[config][paths]") {
  auto const path = default_credentials_path(std::nullopt, std::nullopt, std::nullopt);
  REQUIRE_FALSE(path.has_value());
  CHECK(path.error().m_kind == qoderz::error_kind::configuration);
  CHECK(path.error().m_message.find("QODERZ_CREDENTIALS") != std::string::npos);
  CHECK(path.error().m_message.find("XDG_CONFIG_HOME") != std::string::npos);
}

TEST_CASE("an explicit credential path wins over the environment", "[config][paths]") {
  auto const path = resolve_credentials_path(text("/tmp/explicit.json"), text("/tmp/env.json"),
                                             text("/xdg"), text("/home/ada"), text("/app"));
  REQUIRE(path.has_value());
  CHECK(*path == std::filesystem::path{"/tmp/explicit.json"});
}

TEST_CASE("the environment credential path wins over XDG", "[config][paths]") {
  auto const path = resolve_credentials_path(std::nullopt, text("/tmp/env.json"), text("/xdg"),
                                             text("/home/ada"), text("/app"));
  REQUIRE(path.has_value());
  CHECK(*path == std::filesystem::path{"/tmp/env.json"});
}

TEST_CASE("blank credential locations count as unset", "[config][paths]") {
  auto const path = resolve_credentials_path(text("   "), text(""), std::nullopt, text("/home/ada"),
                                             std::nullopt);
  REQUIRE(path.has_value());
  CHECK(*path == std::filesystem::path{"/home/ada/.config/qoderz/credentials.json"});
}

TEST_CASE("resolving a credential path fails when nothing is configured", "[config][paths]") {
  auto const path = resolve_credentials_path(std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt);
  REQUIRE_FALSE(path.has_value());
  CHECK(path.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("ensure_private_directory creates a 0700 directory", "[config][paths]") {
  temp_dir dir;
  auto const target = dir.file("private");

  auto const ensured = ensure_private_directory(target);
  REQUIRE(ensured.has_value());
  CHECK(std::filesystem::is_directory(target));
#if !defined(_WIN32)
  CHECK(mode_of(target) == 0700);
#endif
}

TEST_CASE("ensure_private_directory creates missing parents", "[config][paths]") {
  temp_dir dir;
  auto const target = dir.file("a").append("b").append("c");

  REQUIRE(ensure_private_directory(target).has_value());
  CHECK(std::filesystem::is_directory(target));
#if !defined(_WIN32)
  CHECK(mode_of(target) == 0700);
#endif
}

TEST_CASE("ensure_private_directory tightens an existing directory", "[config][paths]") {
  temp_dir dir;
  auto const target = dir.file("loose");
  REQUIRE(std::filesystem::create_directories(target));

  REQUIRE(ensure_private_directory(target).has_value());
#if !defined(_WIN32)
  CHECK(mode_of(target) == 0700);
#endif
}

TEST_CASE("ensure_private_directory rejects an existing file", "[config][paths]") {
  temp_dir dir;
  auto const regular = dir.file("credentials.json");
  {
    std::ofstream stream{regular};
    stream << "{}";
  }

  auto const refused = ensure_private_directory(regular);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().m_kind == qoderz::error_kind::configuration);
  CHECK(std::filesystem::is_regular_file(regular));
}

TEST_CASE("ensure_private_directory rejects an empty path", "[config][paths]") {
  auto const ensured = ensure_private_directory(std::filesystem::path{});
  REQUIRE_FALSE(ensured.has_value());
  CHECK(ensured.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("require_directory creates a missing directory as 0700", "[config][paths]") {
  temp_dir dir;
  auto const target = dir.file("a").append("b");

  REQUIRE(require_directory(target).has_value());
  CHECK(std::filesystem::is_directory(target));
#if !defined(_WIN32)
  CHECK(mode_of(target) == 0700);
#endif
}

TEST_CASE("require_directory leaves the mode of an existing directory alone", "[config][paths]") {
  temp_dir dir;
  auto const shared = dir.file("shared");
  REQUIRE(std::filesystem::create_directories(shared));
#if !defined(_WIN32)
  REQUIRE(::chmod(shared.c_str(), 0755) == 0);
  REQUIRE(mode_of(shared) == 0755);
#endif

  REQUIRE(require_directory(shared).has_value());
#if !defined(_WIN32)
  CHECK(mode_of(shared) == 0755);
#endif
}

TEST_CASE("require_directory rejects a file and an empty path", "[config][paths]") {
  temp_dir dir;
  auto const regular = dir.file("credentials.json");
  {
    std::ofstream stream{regular};
    stream << "{}";
  }

  auto const refused = require_directory(regular);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().m_kind == qoderz::error_kind::configuration);

  auto const blank = require_directory(std::filesystem::path{});
  REQUIRE_FALSE(blank.has_value());
  CHECK(blank.error().m_kind == qoderz::error_kind::configuration);
}
