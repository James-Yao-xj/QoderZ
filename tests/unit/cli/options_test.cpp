#include "cli/options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

namespace {

using qoderz::cli::env_options;
using qoderz::cli::global_options;
using qoderz::cli::read_env;

env_options env_of(std::map<std::string, std::string> const& values) {
  return read_env(values);
}

/// A `global_options` with only the named field set, so every literal names all four members.
global_options site_of(std::string site) {
  global_options options;
  options.m_site = std::move(site);
  return options;
}

global_options credentials_of(std::string path) {
  global_options options;
  options.m_credentials = std::move(path);
  return options;
}

global_options level_of(std::string level) {
  global_options options;
  options.m_log_level = std::move(level);
  return options;
}

} // namespace

TEST_CASE("blank environment values count as unset", "[cli][options]") {
  auto const env = env_of({{"QODERZ_SITE", "   "},
                           {"QODERZ_CREDENTIALS", ""},
                           {"QODERZ_LOG_LEVEL", "debug"},
                           {"NO_COLOR", ""}});
  CHECK_FALSE(env.m_site.has_value());
  CHECK_FALSE(env.m_credentials.has_value());
  CHECK(env.m_log_level == "debug");
  // NO_COLOR only disables color when it holds a value; an empty variable means "unset".
  CHECK_FALSE(env.m_no_color);

  auto const colored = env_of({{"NO_COLOR", "1"}});
  CHECK(colored.m_no_color);
}

TEST_CASE("every documented environment variable is read", "[cli][options]") {
  auto const env = env_of({{"QODERZ_SITE", "qoder"},
                           {"QODERZ_CREDENTIALS", "/tmp/creds.json"},
                           {"QODERZ_LISTEN", "http://127.0.0.1:9000"},
                           {"QODERZ_API_KEY", "key-not-real"},
                           {"QODERZ_ACCOUNT", "work"},
                           {"QODERZ_LOG_LEVEL", "warn"},
                           {"QODERZ_UPSTREAM", "http://127.0.0.1:8787"},
                           {"QODERZ_PAT", "pt-not-real"},
                           {"QODERZ_THREADS", "3"},
                           {"XDG_CONFIG_HOME", "/tmp/xdg"},
                           {"HOME", "/tmp/home"},
                           {"APPDATA", "/tmp/appdata"}});
  CHECK(env.m_site == "qoder");
  CHECK(env.m_credentials == "/tmp/creds.json");
  CHECK(env.m_listen == "http://127.0.0.1:9000");
  CHECK(env.m_api_key == "key-not-real");
  CHECK(env.m_account == "work");
  CHECK(env.m_log_level == "warn");
  CHECK(env.m_upstream == "http://127.0.0.1:8787");
  CHECK(env.m_pat == "pt-not-real");
  CHECK(env.m_threads == "3");
  CHECK(env.m_xdg_config_home == "/tmp/xdg");
  CHECK(env.m_home == "/tmp/home");
  CHECK(env.m_app_data == "/tmp/appdata");
  CHECK_FALSE(env.m_no_color);
}

TEST_CASE("the command line beats the environment and the environment beats the default",
          "[cli][options]") {
  auto const env = env_of({{"QODERZ_SITE", "qoder"}});

  auto const from_env = qoderz::cli::resolve_site_definition(global_options{}, env);
  REQUIRE(from_env.has_value());
  CHECK((*from_env)->m_id == "qoder");

  auto const from_cli = qoderz::cli::resolve_site_definition(site_of("qoder-cn"), env);
  REQUIRE(from_cli.has_value());
  CHECK((*from_cli)->m_id == "qoder-cn");

  auto const fallback = qoderz::cli::resolve_site_definition(global_options{}, env_options{});
  REQUIRE(fallback.has_value());
  CHECK((*fallback)->m_id == "qoder-cn");
}

TEST_CASE("an unknown site is a configuration error listing the table", "[cli][options]") {
  auto const unknown = qoderz::cli::resolve_site_definition(site_of("nowhere"), env_options{});
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("the credential path follows the documented priority", "[cli][options]") {
  auto const env = env_of({{"QODERZ_CREDENTIALS", "/tmp/env.json"},
                           {"XDG_CONFIG_HOME", "/tmp/xdg"},
                           {"HOME", "/tmp/home"}});

  auto const explicit_path =
      qoderz::cli::resolve_credentials_path(credentials_of("/tmp/cli.json"), env);
  REQUIRE(explicit_path.has_value());
  CHECK(explicit_path->string() == "/tmp/cli.json");

  auto const from_env = qoderz::cli::resolve_credentials_path(global_options{}, env);
  REQUIRE(from_env.has_value());
  CHECK(from_env->string() == "/tmp/env.json");

  auto const from_xdg = qoderz::cli::resolve_credentials_path(
      global_options{}, env_of({{"XDG_CONFIG_HOME", "/tmp/xdg"}}));
  REQUIRE(from_xdg.has_value());
  CHECK(from_xdg->string() == "/tmp/xdg/qoderz/credentials.json");
}

TEST_CASE("the log level is parsed and applied from either source", "[cli][options]") {
  auto const from_cli =
      qoderz::cli::apply_log_level(level_of("debug"), env_of({{"QODERZ_LOG_LEVEL", "error"}}));
  REQUIRE(from_cli.has_value());
  CHECK(*from_cli == qoderz::log_level::debug);
  CHECK(qoderz::current_log_level() == qoderz::log_level::debug);

  auto const from_env =
      qoderz::cli::apply_log_level(global_options{}, env_of({{"QODERZ_LOG_LEVEL", "warn"}}));
  REQUIRE(from_env.has_value());
  CHECK(*from_env == qoderz::log_level::warn);

  auto const fallback = qoderz::cli::apply_log_level(global_options{}, env_options{});
  REQUIRE(fallback.has_value());
  CHECK(*fallback == qoderz::log_level::info);

  auto const bad = qoderz::cli::apply_log_level(level_of("loud"), env_options{});
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().m_kind == qoderz::error_kind::configuration);

  qoderz::set_log_level(qoderz::log_level::info);
}

TEST_CASE("QODERZ_UPSTREAM only accepts a loopback origin", "[cli][options]") {
  auto const absent = qoderz::cli::upstream_origin(env_options{});
  REQUIRE(absent.has_value());
  CHECK_FALSE(absent->has_value());

  for (auto const* good : {"http://127.0.0.1:8787", "http://localhost:1", "http://[::1]:9000"}) {
    auto const parsed = qoderz::cli::upstream_origin(env_of({{"QODERZ_UPSTREAM", good}}));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->has_value());
    CHECK(**parsed == good);
  }

  for (auto const* bad :
       {"http://example.com:80", "https://127.0.0.1:1", "http://127.0.0.1:1/path", "not a url"}) {
    auto const parsed = qoderz::cli::upstream_origin(env_of({{"QODERZ_UPSTREAM", bad}}));
    CHECK_FALSE(parsed.has_value());
    if (!parsed.has_value()) {
      CHECK(parsed.error().m_kind == qoderz::error_kind::configuration);
    }
  }
}

TEST_CASE("the upstream override is applied after the VPC is resolved", "[cli][options]") {
  auto const site = qoderz::config::find_site("qoder-cn");
  REQUIRE(site.has_value());

  auto const vpc = qoderz::cli::resolve_site_with_vpc(**site, "acme", std::nullopt);
  REQUIRE(vpc.has_value());
  CHECK(vpc->m_vpc == "acme");
  CHECK(vpc->m_openapi_base.find("acme") != std::string::npos);

  auto const overridden =
      qoderz::cli::resolve_site_with_vpc(**site, "acme", std::string{"http://127.0.0.1:1234"});
  REQUIRE(overridden.has_value());
  CHECK(overridden->m_vpc == "acme");
  CHECK(overridden->m_web_base == "http://127.0.0.1:1234");
  CHECK(overridden->m_openapi_base == "http://127.0.0.1:1234");
  CHECK(overridden->m_api_base == "http://127.0.0.1:1234");
}

TEST_CASE("the listen address only accepts http://host:port", "[cli][options]") {
  auto const plain = qoderz::cli::parse_listen("http://127.0.0.1:8788");
  REQUIRE(plain.has_value());
  CHECK(plain->m_host == "127.0.0.1");
  CHECK(plain->m_port == 8788);

  auto const ephemeral = qoderz::cli::parse_listen("http://127.0.0.1:0");
  REQUIRE(ephemeral.has_value());
  CHECK(ephemeral->m_port == 0);

  auto const defaulted = qoderz::cli::parse_listen("http://127.0.0.1");
  REQUIRE(defaulted.has_value());
  CHECK(defaulted->m_port == 80);

  for (auto const* bad : {"https://127.0.0.1:8788", "http://127.0.0.1:8788/v1",
                          "http://127.0.0.1:8788?x=1", "127.0.0.1:8788", "http://:8788"}) {
    auto const parsed = qoderz::cli::parse_listen(bad);
    CHECK_FALSE(parsed.has_value());
  }
}

TEST_CASE("threads default to min(hardware, 4) and must be positive", "[cli][options]") {
  CHECK(qoderz::cli::default_threads() >= 1);
  CHECK(qoderz::cli::default_threads() <= 4);

  auto const from_env = qoderz::cli::resolve_threads(env_of({{"QODERZ_THREADS", "2"}}));
  REQUIRE(from_env.has_value());
  CHECK(*from_env == 2);

  auto const fallback = qoderz::cli::resolve_threads(env_options{});
  REQUIRE(fallback.has_value());
  CHECK(*fallback == qoderz::cli::default_threads());

  CHECK_FALSE(qoderz::cli::resolve_threads(env_of({{"QODERZ_THREADS", "0"}})).has_value());
  CHECK_FALSE(qoderz::cli::resolve_threads(env_of({{"QODERZ_THREADS", "many"}})).has_value());
}

TEST_CASE("the body limit is a positive number", "[cli][options]") {
  auto const parsed = qoderz::cli::parse_body_limit("1048576");
  REQUIRE(parsed.has_value());
  CHECK(*parsed == 1048576);
  CHECK_FALSE(qoderz::cli::parse_body_limit("0").has_value());
  CHECK_FALSE(qoderz::cli::parse_body_limit("-1").has_value());
  CHECK_FALSE(qoderz::cli::parse_body_limit("big").has_value());
}
