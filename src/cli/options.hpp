#pragma once

#include "cli/app.hpp"
#include "common/error.hpp"
#include "common/log.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"
#include "config/paths.hpp"
#include "config/site.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::cli {

/// The environment of `docs/design/cli.md` §10. A blank value counts as unset, which is why every
/// member is optional rather than a possibly-empty string.
struct env_options {
  std::optional<std::string> m_site;
  std::optional<std::string> m_credentials;
  std::optional<std::string> m_listen;
  std::optional<std::string> m_api_key;
  std::optional<std::string> m_account;
  std::optional<std::string> m_log_level;
  std::optional<std::string> m_upstream;
  std::optional<std::string> m_pat;
  std::optional<std::string> m_threads;
  std::optional<std::string> m_xdg_config_home;
  std::optional<std::string> m_home;
  std::optional<std::string> m_app_data;
  bool m_no_color = false;
};

[[nodiscard]] env_options read_env(std::map<std::string, std::string> const& env);

/// The four global flags as CLI11 bound them. A blank value is treated as "not given", the same
/// rule the environment follows.
struct global_flags {
  std::string m_site;
  std::string m_credentials;
  std::string m_log_level;
  bool m_no_color = false;
};

struct global_options {
  std::optional<std::string> m_site;
  std::optional<std::string> m_credentials;
  std::optional<std::string> m_log_level;
  bool m_no_color = false;
};

[[nodiscard]] global_options make_global_options(global_flags const& flags);

/// What one command sees: the process facts, the environment and the global flags.
struct command_context {
  app_context& m_app;
  env_options m_env;
  global_options m_cli;
};

[[nodiscard]] bool is_interactive(app_context const& app) noexcept;

/// ANSI color is on only for a terminal on stdout that did not ask for `NO_COLOR`.
[[nodiscard]] bool color_enabled(command_context const& context) noexcept;

/// `--site`, then `QODERZ_SITE`, then `qoder-cn`; an unknown id is a configuration error listing
/// the site table.
[[nodiscard]] result<config::site_definition const*>
resolve_site_definition(global_options const& cli, env_options const& env);

/// `--credentials`, then `QODERZ_CREDENTIALS`, then the XDG location of `config::paths`.
[[nodiscard]] result<std::filesystem::path> resolve_credentials_path(global_options const& cli,
                                                                     env_options const& env);

/// `--log-level`, then `QODERZ_LOG_LEVEL`, then `info`; parsing sets the global threshold so every
/// later log line honours it.
[[nodiscard]] result<log_level> apply_log_level(global_options const& cli, env_options const& env);

/// `QODERZ_UPSTREAM` when it names a loopback `http` origin. It is applied after the VPC has been
/// resolved, so `--site` and `--vpc` can never drop the override.
[[nodiscard]] result<std::optional<std::string>> upstream_origin(env_options const& env);

[[nodiscard]] result<config::resolved_site>
resolve_site_with_vpc(config::site_definition const& site, std::string_view vpc,
                      std::optional<std::string> const& upstream);

struct listen_address {
  std::string m_host;
  std::uint16_t m_port = 8788;
};

/// Only `http://host:port` is accepted; a path, a query, `https` or a bad port is a configuration
/// error, because the gateway is meant to sit behind a local reverse proxy.
[[nodiscard]] result<listen_address> parse_listen(std::string_view text);

/// `min(hardware_concurrency, 4)`, never zero.
[[nodiscard]] std::size_t default_threads() noexcept;

[[nodiscard]] result<unsigned> parse_threads(std::string_view text);

[[nodiscard]] result<std::size_t> parse_body_limit(std::string_view text);

/// `QODERZ_THREADS` else `default_threads()`; the command line flag is the caller's own option.
[[nodiscard]] result<unsigned> resolve_threads(env_options const& env);

/// One account a selector resolved to, with the site it belongs to.
struct account_candidate {
  std::string m_site;
  std::string m_key;
  config::account_record const* m_account = nullptr;
};

/// Matches `selector` across every account of `sites`: exact name, then uid, then email, then a
/// unique prefix, all ignoring ASCII case.
[[nodiscard]] std::vector<account_candidate>
match_accounts_across(config::credential_file const& file, std::span<std::string_view const> sites,
                      std::string_view selector);

/// The single account of `site` per `docs/design/accounts.md` §5: explicit selector, then the site
/// default, then the site's only account. An absent or ambiguous selector on a terminal becomes a
/// numbered choice on stderr; without a terminal it fails and lists the candidates.
[[nodiscard]] result<config::account_record const*>
select_account(config::credential_file const& file, std::string_view site,
               std::optional<std::string> const& selector, command_context const& context);

/// Numbered choice of `labels` on stderr, returning the selected index. `std::nullopt` means there
/// is no terminal, the input ended, or the answer was not one of the numbers.
[[nodiscard]] std::optional<std::size_t> choose_interactive(std::vector<std::string> const& labels,
                                                            std::string_view question,
                                                            app_context const& app);

/// One `[y/N]` question; `true` only for `y` or `yes` (any case) after trimming.
[[nodiscard]] bool confirm(std::string_view question, app_context const& app);

} // namespace qoderz::cli
