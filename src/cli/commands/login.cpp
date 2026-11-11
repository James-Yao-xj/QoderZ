#include "cli/commands/login.hpp"

#include "cli/output.hpp"
#include "common/clock.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "common/strings.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"
#include "qoder/auth/device.hpp"
#include "qoder/auth/pat.hpp"

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <istream>
#include <optional>
#include <string>
#include <utility>

namespace qoderz::cli {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::seconds progress_step{15};

std::string identity_of(config::account_record const& record) {
  if (!record.m_account_id.empty()) {
    return record.m_account_id;
  }
  if (!record.m_uid.empty()) {
    return record.m_uid;
  }
  return record.m_name;
}

long long whole_seconds(std::chrono::steady_clock::duration value) {
  auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(value).count();
  return seconds < 0 ? 0 : seconds;
}

result<config::account_record> sign_in_with_device(command_context& context,
                                                   login_options const& options,
                                                   config::site_definition const& site,
                                                   config::resolved_site const& resolved,
                                                   net::http_client& http, qoderz::clock& time) {
  qoder::auth::device_flow flow{http, site, resolved, time};
  qoder::auth::login_options flow_options;
  flow_options.m_open_browser = !options.m_no_browser;
  flow_options.m_requested_name = options.m_name;

  std::ostream& progress_out = options.m_json ? context.m_app.m_err : context.m_app.m_out;
  bool first_event = true;
  std::chrono::steady_clock::duration reported{};

  auto const progress = [&](qoder::auth::progress_event const& event) {
    if (first_event) {
      first_event = false;
      reported = event.m_elapsed;
      progress_out << event.m_instructions << '\n' << event.m_url << '\n';
      progress_out.flush();
      return;
    }
    if (event.m_elapsed - reported < progress_step) {
      return;
    }
    reported = event.m_elapsed;
    if (!is_interactive(context.m_app)) {
      return;
    }
    auto const left = flow_options.m_timeout - event.m_elapsed;
    context.m_app.m_err << "still waiting for the sign-in (" << whole_seconds(event.m_elapsed)
                        << "s elapsed, " << whole_seconds(left) << "s left)\n";
    context.m_app.m_err.flush();
  };

  boost::asio::io_context io;
  return run_blocking(io, flow.login(flow_options, progress));
}

result<config::account_record> sign_in_with_pat(config::site_definition const& site,
                                                config::resolved_site const& resolved,
                                                std::string const& pat) {
  boost::asio::io_context io;
  net::http_client http{io};
  qoderz::system_clock time;
  return run_blocking(io, qoder::auth::login_with_pat(http, site, resolved, pat, time));
}

result<config::account_record> sign_in(command_context& context, login_options const& options,
                                       std::string const& pat) {
  auto site = resolve_site_definition(context.m_cli, context.m_env);
  if (!site.has_value()) {
    return std::unexpected(std::move(site.error()));
  }
  auto upstream = upstream_origin(context.m_env);
  if (!upstream.has_value()) {
    return std::unexpected(std::move(upstream.error()));
  }
  auto resolved = resolve_site_with_vpc(**site, options.m_vpc, *upstream);
  if (!resolved.has_value()) {
    return std::unexpected(std::move(resolved.error()));
  }

  if (pat.empty()) {
    boost::asio::io_context io;
    net::http_client http{io};
    qoderz::system_clock time;
    return sign_in_with_device(context, options, **site, *resolved, http, time);
  }
  return sign_in_with_pat(**site, *resolved, pat);
}

result<void> store_account(command_context& context, login_options const& options,
                           config::account_record record) {
  auto path = resolve_credentials_path(context.m_cli, context.m_env);
  if (!path.has_value()) {
    return std::unexpected(std::move(path.error()));
  }
  config::credential_store store{*path};
  auto file = store.load();
  if (!file.has_value()) {
    return std::unexpected(std::move(file.error()));
  }

  std::optional<std::string> requested;
  if (!options.m_name.empty()) {
    requested = options.m_name;
  }
  auto stored = config::upsert_account(*file, std::move(record), requested, options.m_replace);
  if (!stored.has_value()) {
    return std::unexpected(std::move(stored.error()));
  }
  auto const& account = **stored;
  auto const* default_key = [&]() -> std::string const* {
    auto const found = file->m_defaults.find(account.m_site);
    return found == file->m_defaults.end() ? nullptr : &found->second;
  }();
  bool const is_default = default_key != nullptr && *default_key == account.key();

  auto saved = store.save(*file);
  if (!saved.has_value()) {
    return std::unexpected(std::move(saved.error()));
  }

  if (options.m_json) {
    write_json(context.m_app.m_out, json{{"site", account.m_site},
                                         {"name", account.m_name},
                                         {"vpc", account.m_vpc},
                                         {"email", account.m_email},
                                         {"uid", account.m_uid},
                                         {"account_id", account.m_account_id},
                                         {"default", is_default}});
    return {};
  }

  context.m_app.m_out << "Signed in to " << account.m_site << " as " << identity_of(account)
                      << " (account '" << account.m_name << "').\n";
  if (is_default) {
    context.m_app.m_out << account.m_name << " is now the default account for " << account.m_site
                        << ".\n";
  }
  context.m_app.m_out.flush();
  return {};
}

} // namespace

/// The only mutually exclusive pair of `docs/design/cli.md` §2 is `--pat` and `--pat-stdin`;
/// `QODERZ_PAT` is a fallback that an explicit flag simply overrides, never a usage error.
result<std::optional<std::string>> resolve_pat(login_options const& options, env_options const& env,
                                               app_context& app) {
  if (!options.m_pat.empty() && options.m_pat_stdin) {
    return std::unexpected(bad_request_error("--pat and --pat-stdin cannot be combined"));
  }
  if (!options.m_pat.empty()) {
    return std::optional<std::string>{options.m_pat};
  }
  if (options.m_pat_stdin) {
    std::string line;
    if (!std::getline(app.m_in, line)) {
      return std::unexpected(
          configuration_error("--pat-stdin could not read a line from standard input"));
    }
    auto const trimmed = strings::trim(line);
    if (trimmed.empty()) {
      return std::unexpected(configuration_error("--pat-stdin read an empty line"));
    }
    return std::optional<std::string>{std::string{trimmed}};
  }
  if (env.m_pat.has_value()) {
    return std::optional<std::string>{*env.m_pat};
  }
  return std::optional<std::string>{};
}

int run_login(command_context& context, login_options const& options) {
  if (!options.m_name.empty()) {
    auto const valid = config::validate_account_name(options.m_name);
    if (!valid.has_value()) {
      return report_error(context.m_app, valid.error());
    }
  }

  bool const explicit_pat = !options.m_pat.empty() || options.m_pat_stdin;
  if (options.m_device && explicit_pat) {
    return report_error(context.m_app, bad_request_error("--device cannot be combined with a PAT"));
  }
  auto pat = resolve_pat(options, context.m_env, context.m_app);
  if (!pat.has_value()) {
    return report_error(context.m_app, pat.error());
  }
  if (options.m_device) {
    // `--device` is a command line flag, so it outranks the `QODERZ_PAT` fallback of §10.
    pat->reset();
  }
  if (pat->has_value()) {
    auto const valid = qoder::auth::validate_pat(**pat);
    if (!valid.has_value()) {
      return report_error(context.m_app, valid.error());
    }
  }

  auto record = sign_in(context, options, pat->value_or(""));
  if (!record.has_value()) {
    return report_error(context.m_app, record.error());
  }
  auto stored = store_account(context, options, std::move(*record));
  if (!stored.has_value()) {
    return report_error(context.m_app, stored.error());
  }
  return code_of(exit_code::success);
}

} // namespace qoderz::cli
