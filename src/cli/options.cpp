#include "cli/options.hpp"

#include "common/net/url.hpp"
#include "common/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <thread>
#include <utility>

namespace qoderz::cli {
namespace {

std::optional<std::string> read_value(std::map<std::string, std::string> const& env,
                                      std::string_view name) {
  auto const found = env.find(std::string{name});
  if (found == env.end()) {
    return std::nullopt;
  }
  auto const trimmed = strings::trim(found->second);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return std::string{trimmed};
}

std::optional<std::string> pick(std::optional<std::string> const& first,
                                std::optional<std::string> const& second) {
  return first.has_value() ? first : second;
}

template <class number_type>
result<number_type> parse_number(std::string_view text, std::string_view what) {
  auto const trimmed = strings::trim(text);
  if (trimmed.empty()) {
    return std::unexpected(configuration_error(std::string{what} + " must be a number"));
  }
  number_type value = 0;
  auto const* begin = trimmed.data();
  auto const* end = begin + trimmed.size();
  auto const [parsed, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || parsed != end) {
    return std::unexpected(configuration_error(std::string{what} + " must be a number, got '" +
                                               std::string{trimmed} + "'"));
  }
  return value;
}

std::string join_names(std::vector<std::string> const& names) {
  std::string text;
  for (auto const& name : names) {
    if (!text.empty()) {
      text.append(", ");
    }
    text.append(name);
  }
  return text;
}

std::string join_keys(std::vector<account_candidate> const& candidates) {
  std::string text;
  for (auto const& candidate : candidates) {
    if (!text.empty()) {
      text.append(", ");
    }
    text.append(candidate.m_key);
  }
  return text;
}

} // namespace

env_options read_env(std::map<std::string, std::string> const& env) {
  env_options options;
  options.m_site = read_value(env, "QODERZ_SITE");
  options.m_credentials = read_value(env, "QODERZ_CREDENTIALS");
  options.m_listen = read_value(env, "QODERZ_LISTEN");
  options.m_api_key = read_value(env, "QODERZ_API_KEY");
  options.m_account = read_value(env, "QODERZ_ACCOUNT");
  options.m_log_level = read_value(env, "QODERZ_LOG_LEVEL");
  options.m_upstream = read_value(env, "QODERZ_UPSTREAM");
  options.m_pat = read_value(env, "QODERZ_PAT");
  options.m_threads = read_value(env, "QODERZ_THREADS");
  options.m_xdg_config_home = read_value(env, "XDG_CONFIG_HOME");
  options.m_home = read_value(env, "HOME");
  options.m_app_data = read_value(env, "APPDATA");
  options.m_no_color = read_value(env, "NO_COLOR").has_value();
  return options;
}

global_options make_global_options(global_flags const& flags) {
  auto const value = [](std::string const& text) -> std::optional<std::string> {
    auto const trimmed = strings::trim(text);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    return std::string{trimmed};
  };
  return global_options{.m_site = value(flags.m_site),
                        .m_credentials = value(flags.m_credentials),
                        .m_log_level = value(flags.m_log_level),
                        .m_no_color = flags.m_no_color};
}
bool is_interactive(app_context const& app) noexcept {
  return app.m_stdin_tty && app.m_stdout_tty;
}

bool color_enabled(command_context const& context) noexcept {
  return context.m_app.m_stdout_tty && !context.m_cli.m_no_color && !context.m_env.m_no_color;
}

result<config::site_definition const*> resolve_site_definition(global_options const& cli,
                                                               env_options const& env) {
  auto const id = pick(cli.m_site, env.m_site).value_or(std::string{"qoder-cn"});
  return config::find_site(id);
}

result<std::filesystem::path> resolve_credentials_path(global_options const& cli,
                                                       env_options const& env) {
  return ::qoderz::config::resolve_credentials_path(
      cli.m_credentials, env.m_credentials, env.m_xdg_config_home, env.m_home, env.m_app_data);
}

result<log_level> apply_log_level(global_options const& cli, env_options const& env) {
  auto const text = pick(cli.m_log_level, env.m_log_level);
  if (!text.has_value()) {
    set_log_level(log_level::info);
    return log_level::info;
  }
  auto const parsed = parse_log_level(*text);
  if (!parsed.has_value()) {
    return std::unexpected(configuration_error(
        "unknown log level '" + *text + "'; use trace, debug, info, warn, error or critical"));
  }
  set_log_level(*parsed);
  return *parsed;
}

result<std::optional<std::string>> upstream_origin(env_options const& env) {
  if (!env.m_upstream.has_value()) {
    return std::optional<std::string>{};
  }
  auto const& value = *env.m_upstream;
  auto const parsed = net::parse_url(value);
  if (!parsed.has_value()) {
    return std::unexpected(configuration_error(
        "QODERZ_UPSTREAM must be a loopback origin such as http://127.0.0.1:8787: " +
        parsed.error().m_message));
  }
  if (parsed->m_scheme != "http") {
    return std::unexpected(configuration_error("QODERZ_UPSTREAM only accepts the http scheme"));
  }
  if (!net::is_loopback_host(parsed->m_host)) {
    return std::unexpected(
        configuration_error("QODERZ_UPSTREAM must point at 127.0.0.0/8, ::1 or localhost, not '" +
                            parsed->m_host + "'"));
  }
  if (parsed->m_target != "/") {
    return std::unexpected(
        configuration_error("QODERZ_UPSTREAM must be a bare origin without a path or query"));
  }
  return std::optional<std::string>{value};
}

result<config::resolved_site> resolve_site_with_vpc(config::site_definition const& site,
                                                    std::string_view vpc,
                                                    std::optional<std::string> const& upstream) {
  auto resolved = config::resolve_site(site, vpc);
  if (!resolved.has_value()) {
    return std::unexpected(std::move(resolved.error()));
  }
  if (!upstream.has_value()) {
    return std::move(*resolved);
  }
  return config::apply_loopback_override(*resolved, *upstream);
}

result<listen_address> parse_listen(std::string_view text) {
  auto const parsed = net::parse_url(text);
  if (!parsed.has_value()) {
    return std::unexpected(configuration_error("invalid listen address '" + std::string{text} +
                                               "': " + parsed.error().m_message));
  }
  if (parsed->m_scheme != "http") {
    return std::unexpected(configuration_error(
        "the listen address must be http://host:port; TLS belongs to a reverse proxy"));
  }
  if (parsed->m_target != "/") {
    return std::unexpected(
        configuration_error("the listen address must not carry a path or a query"));
  }
  auto const port = parse_number<unsigned>(parsed->m_port, "the listen port");
  if (!port.has_value()) {
    return std::unexpected(std::move(port.error()));
  }
  if (*port > 65535) {
    return std::unexpected(configuration_error("the listen port must be between 0 and 65535"));
  }
  return listen_address{.m_host = parsed->m_host, .m_port = static_cast<std::uint16_t>(*port)};
}

std::size_t default_threads() noexcept {
  auto const hardware = std::thread::hardware_concurrency();
  auto const limit = static_cast<unsigned>(std::max(1U, hardware));
  return std::min<std::size_t>(limit, 4);
}

result<unsigned> parse_threads(std::string_view text) {
  auto const parsed = parse_number<unsigned>(text, "--threads");
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (*parsed == 0) {
    return std::unexpected(configuration_error("--threads must be at least 1"));
  }
  return *parsed;
}

result<unsigned> resolve_threads(env_options const& env) {
  if (!env.m_threads.has_value()) {
    return static_cast<unsigned>(default_threads());
  }
  return parse_threads(*env.m_threads);
}

result<std::size_t> parse_body_limit(std::string_view text) {
  auto const parsed = parse_number<std::size_t>(text, "--max-body-bytes");
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (*parsed == 0) {
    return std::unexpected(configuration_error("--max-body-bytes must be at least 1"));
  }
  return *parsed;
}

std::vector<account_candidate> match_accounts_across(config::credential_file const& file,
                                                     std::span<std::string_view const> sites,
                                                     std::string_view selector) {
  std::vector<account_candidate> matches;
  auto const exact = [&](auto&& field_of) {
    for (auto const& site : sites) {
      for (auto const* account : config::accounts_for_site(file, site)) {
        if (strings::equals_ignore_case(field_of(*account), selector) &&
            !field_of(*account).empty()) {
          matches.push_back(account_candidate{std::string{site}, account->key(), account});
        }
      }
    }
  };
  exact([](config::account_record const& record) { return record.m_name; });
  if (!matches.empty()) {
    return matches;
  }
  exact([](config::account_record const& record) { return record.m_uid; });
  if (!matches.empty()) {
    return matches;
  }
  exact([](config::account_record const& record) { return record.m_email; });
  if (!matches.empty()) {
    return matches;
  }

  auto const prefix = [&](auto&& field_of) {
    for (auto const& site : sites) {
      for (auto const* account : config::accounts_for_site(file, site)) {
        auto const& field = field_of(*account);
        if (!field.empty() && field.size() >= selector.size() &&
            strings::equals_ignore_case(field.substr(0, selector.size()), selector)) {
          matches.push_back(account_candidate{std::string{site}, account->key(), account});
        }
      }
    }
  };
  prefix([](config::account_record const& record) { return record.m_name; });
  if (!matches.empty()) {
    return matches;
  }
  prefix([](config::account_record const& record) { return record.m_uid; });
  if (!matches.empty()) {
    return matches;
  }
  prefix([](config::account_record const& record) { return record.m_email; });
  return matches;
}

std::optional<std::size_t> choose_interactive(std::vector<std::string> const& labels,
                                              std::string_view question, app_context const& app) {
  if (!is_interactive(app)) {
    return std::nullopt;
  }
  app.m_err << question << '\n';
  for (std::size_t index = 0; index < labels.size(); ++index) {
    app.m_err << "  " << (index + 1) << ") " << labels[index] << '\n';
  }
  app.m_err << "select [1-" << labels.size() << ", default 1]: ";
  app.m_err.flush();

  std::string line;
  if (!std::getline(app.m_in, line)) {
    return std::nullopt;
  }
  auto const trimmed = strings::trim(line);
  if (trimmed.empty()) {
    return labels.empty() ? std::nullopt : std::optional<std::size_t>{0};
  }
  auto const parsed = parse_number<unsigned>(trimmed, "the selection");
  if (!parsed.has_value() || *parsed < 1 || *parsed > labels.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(*parsed - 1);
}

bool confirm(std::string_view question, app_context const& app) {
  app.m_err << question << " [y/N]: ";
  app.m_err.flush();
  std::string line;
  if (!std::getline(app.m_in, line)) {
    return false;
  }
  auto const answer = strings::ascii_lower_copy(strings::trim(line));
  return answer == "y" || answer == "yes";
}

result<config::account_record const*> select_account(config::credential_file const& file,
                                                     std::string_view site,
                                                     std::optional<std::string> const& selector,
                                                     command_context const& context) {
  auto const accounts = config::accounts_for_site(file, site);
  if (accounts.empty()) {
    return std::unexpected(make_error(error_kind::auth, "not signed in; run: qoderz login"));
  }

  std::vector<std::string> names;
  names.reserve(accounts.size());
  for (auto const* account : accounts) {
    names.push_back(account->m_name);
  }

  if (!selector.has_value()) {
    if (auto const* fallback = config::default_account(file, site); fallback != nullptr) {
      return fallback;
    }
    if (accounts.size() == 1) {
      return accounts.front();
    }
    if (auto const chosen = choose_interactive(
            names, "several accounts for " + std::string{site} + " and no default:", context.m_app);
        chosen.has_value()) {
      return accounts[*chosen];
    }
    return std::unexpected(
        configuration_error("no default account for " + std::string{site} +
                            "; pass --account <name> or run: qoderz account use <name>"));
  }

  std::string_view const sites[] = {site};
  auto const matches = match_accounts_across(file, sites, *selector);
  if (matches.empty()) {
    return std::unexpected(configuration_error("no account matches '" + *selector + "' for " +
                                               std::string{site} +
                                               "; available: " + join_names(names)));
  }
  if (matches.size() == 1) {
    return matches.front().m_account;
  }

  std::vector<std::string> labels;
  labels.reserve(matches.size());
  for (auto const& match : matches) {
    labels.push_back(match.m_account->m_name);
  }
  if (auto const chosen = choose_interactive(
          labels, "'" + *selector + "' matches " + std::to_string(labels.size()) + " accounts:",
          context.m_app);
      chosen.has_value()) {
    return matches[*chosen].m_account;
  }
  return std::unexpected(configuration_error("'" + *selector + "' is ambiguous for " +
                                             std::string{site} + ": " + join_keys(matches) +
                                             "; use a full name"));
}

} // namespace qoderz::cli
