#include "cli/app.hpp"

#include "cli/commands/account.hpp"
#include "cli/commands/login.hpp"
#include "cli/commands/models.hpp"
#include "cli/commands/serve/serve.hpp"
#include "cli/commands/usage.hpp"
#include "cli/options.hpp"
#include "cli/output.hpp"
#include "common/log.hpp"
#include "common/version.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"

#include <CLI/CLI.hpp>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::cli {
namespace {

constexpr std::string_view program_name = "qoderz";
constexpr std::string_view program_description =
    "Qoder to OpenAI-compatible gateway. The site defaults to qoder-cn; pass --site qoder for the "
    "international site.";

void add_global_flags(CLI::App& app, global_flags& flags) {
  app.add_option("--site", flags.m_site, "Site: qoder | qoder-cn");
  app.add_option("--credentials", flags.m_credentials, "Credential file path");
  app.add_option("--log-level", flags.m_log_level,
                 "Log level: trace | debug | info | warn | error | critical");
  app.add_flag("--no-color", flags.m_no_color, "Disable ANSI color; NO_COLOR does the same");
}

int print_version(app_context& context) {
  context.m_out << program_name << ' ' << version::string << '\n';
  context.m_out.flush();
  return code_of(exit_code::success);
}

int print_overview(app_context& context, command_context& command) {
  context.m_out << program_name << ' ' << version::string << '\n';
  auto const path = resolve_credentials_path(command.m_cli, command.m_env);
  if (!path.has_value()) {
    context.m_out << "credentials: " << path.error().m_message << '\n';
  } else {
    context.m_out << "credentials: " << path->string() << '\n';
    config::credential_store store{*path};
    auto file = store.load();
    if (!file.has_value()) {
      context.m_out << "accounts: " << file.error().m_message << '\n';
    } else if (file->m_accounts.empty()) {
      context.m_out << "accounts: none; run: qoderz login\n";
    } else {
      for (auto const& site : config::all_sites()) {
        auto const accounts = config::accounts_for_site(*file, site.m_id);
        if (accounts.empty()) {
          continue;
        }
        auto const* fallback = config::default_account(*file, site.m_id);
        context.m_out << "accounts: " << accounts.size() << " for " << site.m_id
                      << " (default: " << (fallback != nullptr ? fallback->m_name : "none")
                      << ")\n";
      }
    }
  }
  context.m_out << "\ncommands:\n"
                   "  qoderz login       sign in (device flow, or --pat-stdin)\n"
                   "  qoderz accounts    list accounts\n"
                   "  qoderz models      print the model catalogue\n"
                   "  qoderz usage       print plan and quota windows\n"
                   "  qoderz serve       start the OpenAI-compatible gateway\n"
                   "  qoderz help        help for one command\n";
  context.m_out.flush();
  return code_of(exit_code::success);
}

int report_parse_error(app_context& context, CLI::App const& app, CLI::Error const& failure) {
  context.m_err << "error: " << failure.what() << '\n';
  context.m_err << "usage: " << app.get_name() << " <command> [options]; run: " << app.get_name()
                << " help\n";
  context.m_err.flush();
  return code_of(exit_code::usage);
}

/// The exit-code table of `docs/design/cli.md` §11, appended to every help screen.
std::string usage_footer() {
  return "\nexit codes:\n"
         "  0  success, --help, --version, no command\n"
         "  1  configuration or internal failure\n"
         "  2  command line usage error\n"
         "  3  authentication failure or expired sign-in\n"
         "  4  upstream failure (quota, busy, rejected)\n"
         "  130 interrupted (Ctrl-C)\n";
}

/// Two or three examples per command, as `docs/design/cli.md` §8 asks for.
std::string examples_of(std::string const& command) {
  if (command == "login") {
    return "\nexamples:\n"
           "  qoderz login --name work\n"
           "  printf '%s' \"$PAT\" | qoderz login --site qoder --pat-stdin --name personal\n";
  }
  if (command == "accounts") {
    return "\nexamples:\n  qoderz accounts\n  qoderz accounts --site qoder --json\n";
  }
  if (command == "account") {
    return "\nexamples:\n  qoderz account use personal\n  qoderz account show work --json\n"
           "  qoderz account remove work --yes\n";
  }
  if (command == "logout") {
    return "\nexamples:\n  qoderz logout\n  qoderz logout --all --yes\n";
  }
  if (command == "models" || command == "usage") {
    return "\nexamples:\n  qoderz " + command + "\n  qoderz " + command +
           " --account work --json\n";
  }
  if (command == "serve") {
    return "\nexamples:\n"
           "  qoderz serve\n"
           "  qoderz serve --accounts work,personal --listen http://127.0.0.1:8788\n"
           "  qoderz serve --account work --api-key-stdin\n";
  }
  if (command == "help") {
    return "\nexamples:\n  qoderz help\n  qoderz help serve\n";
  }
  return {};
}

int run_help(app_context& context, CLI::App& app, std::string const& command) {
  if (command.empty()) {
    context.m_out << app.help() << usage_footer();
    context.m_out.flush();
    return code_of(exit_code::success);
  }
  auto const* sub = app.get_subcommand(command);
  if (sub == nullptr) {
    context.m_err << "error: unknown command '" << command << "'\n";
    context.m_err << "usage: " << app.get_name() << " <command> [options]\n";
    context.m_err.flush();
    return code_of(exit_code::usage);
  }
  context.m_out << sub->help() << examples_of(sub->get_name()) << usage_footer();
  context.m_out.flush();
  return code_of(exit_code::success);
}

/// The deepest subcommand that was named, used to pick the help a failed `--help` should show.
std::string active_command(CLI::App const& app) {
  CLI::App const* current = &app;
  for (;;) {
    CLI::App const* next = nullptr;
    for (auto const* sub : current->get_subcommands()) {
      if (sub->count() > 0) {
        next = sub;
      }
    }
    if (next == nullptr) {
      break;
    }
    current = next;
  }
  return current == &app ? std::string{} : current->get_name();
}

} // namespace

int exit_code_for(error const& value) noexcept {
  switch (value.m_kind) {
  case error_kind::bad_request:
    return code_of(exit_code::usage);
  case error_kind::auth:
    return code_of(exit_code::auth);
  case error_kind::quota:
  case error_kind::busy:
  case error_kind::upstream:
  case error_kind::protocol:
    return code_of(exit_code::upstream);
  case error_kind::canceled:
    return code_of(exit_code::interrupted);
  case error_kind::configuration:
  case error_kind::io:
  case error_kind::internal:
    break;
  }
  return code_of(exit_code::configuration);
}

int report_error(app_context& context, error const& value) {
  context.m_err << "error: " << value.m_message << '\n';
  context.m_err.flush();
  return exit_code_for(value);
}

int run(app_context& context) {
  CLI::App app{std::string{program_description}, std::string{program_name}};
  app.set_version_flag("--version", std::string{program_name} + " " + std::string{version::string});
  app.ignore_case(true);

  global_flags globals;
  add_global_flags(app, globals);

  login_options login;
  auto* login_cmd = app.add_subcommand("login", "Sign in and store an account");
  login_cmd->alias("add");
  add_global_flags(*login_cmd, globals);
  login_cmd->add_option("--name", login.m_name, "Account name (checked before any network call)");
  login_cmd->add_option("--vpc", login.m_vpc, "Qoder CN enterprise VPC instance");
  login_cmd->add_option("--pat", login.m_pat, "Personal access token");
  login_cmd->add_flag("--pat-stdin", login.m_pat_stdin, "Read the PAT from standard input");
  login_cmd->add_flag("--device", login.m_device, "Force the device flow");
  login_cmd->add_flag("--no-browser", login.m_no_browser, "Do not open a browser");
  login_cmd->add_flag("--replace", login.m_replace, "Replace an account of the same name");
  login_cmd->add_flag("--json", login.m_json, "Print the account summary as JSON");

  accounts_options accounts;
  auto* accounts_cmd = app.add_subcommand("accounts", "List accounts");
  accounts_cmd->alias("ls");
  add_global_flags(*accounts_cmd, globals);
  accounts_cmd->add_flag("--all-sites", accounts.m_all_sites, "Every site (the default)");
  accounts_cmd->add_flag("--wide", accounts.m_wide, "Add ACCOUNT_ID and CREATED columns");
  accounts_cmd->add_flag("--json", accounts.m_json, "Print the accounts as JSON");

  account_use_options use;
  account_show_options show;
  remove_options remove;
  auto* account_cmd = app.add_subcommand("account", "Manage one account");
  account_cmd->require_subcommand(1);
  auto* use_cmd = account_cmd->add_subcommand("use", "Set the default account of a site");
  add_global_flags(*use_cmd, globals);
  use_cmd->add_option("selector", use.m_selector, "Account selector")->required();

  auto* show_cmd = account_cmd->add_subcommand("show", "Show one account");
  add_global_flags(*show_cmd, globals);
  show_cmd->add_option("selector", show.m_selector, "Account selector")->required();
  show_cmd->add_flag("--json", show.m_json, "Print the account as JSON");

  auto* remove_cmd = account_cmd->add_subcommand("remove", "Remove accounts");
  add_global_flags(*remove_cmd, globals);
  remove_cmd->add_option("selectors", remove.m_selectors, "Account selectors")->required();
  remove_cmd->add_flag("--yes", remove.m_yes, "Do not ask for confirmation");
  remove_cmd->add_flag("--json", remove.m_json, "Print the removed keys as JSON");

  remove_options logout;
  auto* logout_cmd = app.add_subcommand("logout", "Remove one or more accounts");
  logout_cmd->alias("rm");
  add_global_flags(*logout_cmd, globals);
  logout_cmd->add_option("--account", logout.m_selectors, "Account selector (repeatable)");
  logout_cmd->add_flag("--all", logout.m_all, "Remove every account of the site");
  logout_cmd->add_flag("--yes", logout.m_yes, "Do not ask for confirmation");
  logout_cmd->add_flag("--json", logout.m_json, "Print the removed keys as JSON");

  query_options models_query;
  auto* models_cmd = app.add_subcommand("models", "Print the model catalogue");
  add_global_flags(*models_cmd, globals);
  models_cmd->add_option("--account", models_query.m_account, "Account selector");
  models_cmd->add_flag("--json", models_query.m_json, "Print the catalogue as JSON");

  query_options usage_query;
  auto* usage_cmd = app.add_subcommand("usage", "Print the plan and quota windows");
  add_global_flags(*usage_cmd, globals);
  usage_cmd->add_option("--account", usage_query.m_account, "Account selector");
  usage_cmd->add_flag("--json", usage_query.m_json, "Print the snapshot as JSON");

  serve_options serve;
  auto* serve_cmd = app.add_subcommand("serve", "Start the OpenAI-compatible gateway");
  add_global_flags(*serve_cmd, globals);
  serve_cmd->add_option("--listen", serve.m_listen, "Listen address: http://host:port");
  serve_cmd->add_option("--account", serve.m_account, "Pin one account");
  serve_cmd->add_option("--accounts", serve.m_accounts, "Rotate over a comma separated subset");
  serve_cmd->add_option("--api-key", serve.m_api_key, "Bearer key for /v1/*");
  serve_cmd->add_flag("--api-key-stdin", serve.m_api_key_stdin, "Read the API key from stdin");
  serve_cmd->add_option("--threads", serve.m_threads, "Worker threads");
  serve_cmd->add_option("--max-body-bytes", serve.m_max_body_bytes, "Request body limit");
  serve_cmd->add_flag("--ignore-cooldown", serve.m_ignore_cooldown,
                      "Keep cooling accounts in rotation");
  serve_cmd->add_flag("--allow-public", serve.m_allow_public,
                      "Allow a public listener without a key");
  serve_cmd->add_flag("--json", serve.m_json, "Print the startup summary as JSON");

  auto* version_cmd = app.add_subcommand("version", "Print the version");
  auto* help_cmd = app.add_subcommand("help", "Print help for one command");
  std::string help_target;
  help_cmd->add_option("command", help_target, "Command to describe");

  app.require_subcommand(0, 1);

  // CLI11's vector overload consumes arguments from the back, so hand it a reversed copy.
  std::vector<std::string> reversed{context.m_args.rbegin(), context.m_args.rend()};
  try {
    app.parse(std::move(reversed));
  } catch (CLI::CallForHelp const&) {
    auto const command = active_command(app);
    if (command.empty()) {
      context.m_out << app.help() << usage_footer();
    } else {
      auto const* sub = app.get_subcommand(command);
      context.m_out << sub->help() << examples_of(sub->get_name()) << usage_footer();
    }
    context.m_out.flush();
    return code_of(exit_code::success);
  } catch (CLI::CallForVersion const&) {
    return print_version(context);
  } catch (CLI::ParseError const& failure) {
    return report_parse_error(context, app, failure);
  }

  auto const global = make_global_options(globals);
  auto const env = read_env(context.m_env);
  auto const level = apply_log_level(global, env);
  if (!level.has_value()) {
    return report_error(context, level.error());
  }
  command_context command{.m_app = context, .m_env = env, .m_cli = global};

  bool const any = login_cmd->count() > 0 || accounts_cmd->count() > 0 || use_cmd->count() > 0 ||
                   show_cmd->count() > 0 || remove_cmd->count() > 0 || logout_cmd->count() > 0 ||
                   models_cmd->count() > 0 || usage_cmd->count() > 0 || serve_cmd->count() > 0 ||
                   version_cmd->count() > 0 || help_cmd->count() > 0;
  if (!any) {
    return print_overview(context, command);
  }
  if (version_cmd->count() > 0) {
    return print_version(context);
  }
  if (help_cmd->count() > 0) {
    return run_help(context, app, help_target);
  }
  if (login_cmd->count() > 0) {
    return run_login(command, login);
  }
  if (accounts_cmd->count() > 0) {
    return run_accounts(command, accounts);
  }
  if (use_cmd->count() > 0) {
    return run_account_use(command, use);
  }
  if (show_cmd->count() > 0) {
    return run_account_show(command, show);
  }
  if (remove_cmd->count() > 0) {
    return run_account_remove(command, remove);
  }
  if (logout_cmd->count() > 0) {
    return run_logout(command, logout);
  }
  if (models_cmd->count() > 0) {
    return run_models(command, models_query);
  }
  if (usage_cmd->count() > 0) {
    return run_usage(command, usage_query);
  }
  if (serve_cmd->count() > 0) {
    return run_serve(command, serve);
  }
  return print_overview(context, command);
}

} // namespace qoderz::cli
