#include "cli/commands/account.hpp"

#include "cli/output.hpp"
#include "common/json.hpp"
#include "common/strings.hpp"
#include "config/accounts.hpp"
#include "config/credentials.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::cli {
namespace {

constexpr std::string_view dash = "-";

struct store_session {
  std::filesystem::path m_path;
  config::credential_file m_file;
};

result<store_session> load_store(command_context& context) {
  auto path = resolve_credentials_path(context.m_cli, context.m_env);
  if (!path.has_value()) {
    return std::unexpected(std::move(path.error()));
  }
  config::credential_store store{*path};
  auto file = store.load();
  if (!file.has_value()) {
    return std::unexpected(std::move(file.error()));
  }
  return store_session{.m_path = std::move(*path), .m_file = std::move(*file)};
}

result<void> save_store(store_session& session) {
  config::credential_store store{session.m_path};
  if (session.m_file.m_accounts.empty()) {
    return store.remove_file();
  }
  return store.save(session.m_file);
}

std::string kind_of(config::account_record const& record) {
  bool const device = !record.m_device_token.empty() || !record.m_device_refresh_token.empty();
  bool const pat = record.m_pat_auth || !record.m_pat.empty();
  if (device && pat) {
    return "device+pat";
  }
  if (pat) {
    return "pat";
  }
  return "device";
}

bool is_default_account(config::credential_file const& file, config::account_record const& record) {
  auto const* fallback = config::default_account(file, record.m_site);
  return fallback != nullptr && fallback->key() == record.key();
}

std::optional<std::int64_t> positive(std::int64_t value) {
  if (value <= 0) {
    return std::nullopt;
  }
  return value;
}

json account_json(config::credential_file const& file, config::account_record const& record) {
  return json{{"name", record.m_name},
              {"site", record.m_site},
              {"vpc", record.m_vpc},
              {"email", record.m_email},
              {"uid", record.m_uid},
              {"account_id", record.m_account_id},
              {"expires_at", format_iso8601(positive(record.m_expires_at))},
              {"kind", kind_of(record)},
              {"default", is_default_account(file, record)}};
}

/// Accounts of `site` in the base order of `docs/design/accounts.md` §4.
std::vector<config::account_record const*> ordered_accounts(config::credential_file const& file,
                                                            std::string_view site) {
  return config::accounts_for_site(file, site);
}

/// Every site mentioned by the file, in site-table order; an unknown site id comes last.
std::vector<std::string> sites_of(config::credential_file const& file) {
  std::vector<std::string> sites;
  for (auto const& entry : config::all_sites()) {
    for (auto const& account : file.m_accounts) {
      if (account.second.m_site == entry.m_id &&
          std::find(sites.begin(), sites.end(), account.second.m_site) == sites.end()) {
        sites.emplace_back(account.second.m_site);
        break;
      }
    }
  }
  for (auto const& account : file.m_accounts) {
    auto const& site = account.second.m_site;
    if (std::find(sites.begin(), sites.end(), site) == sites.end()) {
      sites.push_back(site);
    }
  }
  return sites;
}

error missing_accounts_error(std::string_view site) {
  return make_error(error_kind::auth,
                    "no accounts for " + std::string{site} + "; run: qoderz login");
}

} // namespace

int run_accounts(command_context& context, accounts_options const& options) {
  auto session = load_store(context);
  if (!session.has_value()) {
    return report_error(context.m_app, session.error());
  }

  std::optional<std::string> filter;
  if (context.m_cli.m_site.has_value()) {
    if (options.m_all_sites) {
      return report_error(context.m_app,
                          bad_request_error("--site and --all-sites cannot be combined"));
    }
    auto const site = config::find_site(*context.m_cli.m_site);
    if (!site.has_value()) {
      return report_error(context.m_app, site.error());
    }
    filter = std::string{(*site)->m_id};
  }

  std::vector<config::account_record const*> selected;
  for (auto const& site : sites_of(session->m_file)) {
    if (filter.has_value() && *filter != site) {
      continue;
    }
    for (auto const* account : ordered_accounts(session->m_file, site)) {
      selected.push_back(account);
    }
  }

  if (options.m_json) {
    json array = json::array();
    for (auto const* account : selected) {
      array.push_back(account_json(session->m_file, *account));
    }
    write_json(context.m_app.m_out, array);
    return code_of(exit_code::success);
  }

  if (selected.empty()) {
    context.m_app.m_err << "no accounts yet; run: qoderz login\n";
    return code_of(exit_code::success);
  }

  table view;
  view.m_columns = {{.m_header = "NAME"}, {.m_header = "SITE"},   {.m_header = "EMAIL"},
                    {.m_header = "UID"},  {.m_header = "VPC"},    {.m_header = "EXPIRES"},
                    {.m_header = "KIND"}, {.m_header = "DEFAULT"}};
  if (options.m_wide) {
    view.m_columns.push_back({.m_header = "ACCOUNT_ID"});
    view.m_columns.push_back({.m_header = "CREATED"});
  }

  bool const color = color_enabled(context);
  for (auto const* account : selected) {
    bool const fallback = is_default_account(session->m_file, *account);
    // §3: the default account also carries a `*` after its name; the DEFAULT column stays a plain
    // bit, and `--json` keeps the bare name because its `default` field is already a boolean.
    std::string const name = fallback ? account->m_name + "*" : account->m_name;
    std::vector<std::string> row;
    row.push_back(fallback ? paint(tone::bold, name, color) : name);
    row.push_back(account->m_site);
    row.push_back(account->m_email);
    row.push_back(account->m_uid);
    row.push_back(account->m_vpc);
    row.push_back(format_local_time(positive(account->m_expires_at)));
    row.push_back(kind_of(*account));
    row.push_back(fallback ? "*" : std::string{dash});
    if (options.m_wide) {
      row.push_back(account->m_account_id);
      row.push_back(format_local_time(positive(account->m_created_at)));
    }
    view.m_rows.push_back(std::move(row));
  }

  context.m_app.m_out << render(view);
  context.m_app.m_out.flush();
  return code_of(exit_code::success);
}

namespace {

/// Matches a selector across the requested sites, with an interactive tie-break on a terminal.
result<account_candidate> resolve_one(command_context& context, config::credential_file const& file,
                                      std::vector<std::string> const& sites,
                                      std::string_view selector) {
  std::vector<std::string_view> views;
  views.reserve(sites.size());
  for (auto const& site : sites) {
    views.push_back(site);
  }
  auto const matches = match_accounts_across(file, views, selector);
  if (matches.empty()) {
    std::string available;
    for (auto const& site : sites) {
      for (auto const* account : ordered_accounts(file, site)) {
        if (!available.empty()) {
          available.append(", ");
        }
        available.append(account->key());
      }
    }
    return std::unexpected(configuration_error(
        "no account matches '" + std::string{selector} +
        "'; available: " + (available.empty() ? std::string{"none"} : available)));
  }
  if (matches.size() == 1) {
    return matches.front();
  }

  std::vector<std::string> distinct_sites;
  for (auto const& match : matches) {
    if (std::find(distinct_sites.begin(), distinct_sites.end(), match.m_site) ==
        distinct_sites.end()) {
      distinct_sites.push_back(match.m_site);
    }
  }
  if (distinct_sites.size() > 1) {
    std::string keys;
    for (auto const& match : matches) {
      if (!keys.empty()) {
        keys.append(", ");
      }
      keys.append(match.m_key);
    }
    return std::unexpected(configuration_error("'" + std::string{selector} +
                                               "' matches accounts on several sites (" + keys +
                                               "); pass --site <id>"));
  }

  std::vector<std::string> labels;
  for (auto const& match : matches) {
    labels.push_back(match.m_account->m_name);
  }
  if (auto const chosen = choose_interactive(labels,
                                             "'" + std::string{selector} + "' matches " +
                                                 std::to_string(labels.size()) + " accounts:",
                                             context.m_app);
      chosen.has_value()) {
    return matches[*chosen];
  }
  std::string keys;
  for (auto const& match : matches) {
    if (!keys.empty()) {
      keys.append(", ");
    }
    keys.append(match.m_key);
  }
  return std::unexpected(configuration_error("'" + std::string{selector} +
                                             "' is ambiguous; matches " + keys +
                                             "; use a full name or --site"));
}

/// The sites a command may look at: the `--site` value when given, otherwise every site of the
/// file.
result<std::vector<std::string>> candidate_sites(command_context& context,
                                                 config::credential_file const& file) {
  if (context.m_cli.m_site.has_value()) {
    auto const site = config::find_site(*context.m_cli.m_site);
    if (!site.has_value()) {
      return std::unexpected(site.error());
    }
    return std::vector<std::string>{std::string{(*site)->m_id}};
  }
  return sites_of(file);
}

} // namespace

int run_account_use(command_context& context, account_use_options const& options) {
  auto session = load_store(context);
  if (!session.has_value()) {
    return report_error(context.m_app, session.error());
  }
  auto sites = candidate_sites(context, session->m_file);
  if (!sites.has_value()) {
    return report_error(context.m_app, sites.error());
  }
  if (sites->empty()) {
    return report_error(context.m_app,
                        make_error(error_kind::auth, "not signed in; run: qoderz login"));
  }

  auto match = resolve_one(context, session->m_file, *sites, options.m_selector);
  if (!match.has_value()) {
    return report_error(context.m_app, match.error());
  }

  auto const key = match->m_key;
  auto set = config::set_default_account(session->m_file, key);
  if (!set.has_value()) {
    return report_error(context.m_app, set.error());
  }
  auto saved = save_store(*session);
  if (!saved.has_value()) {
    return report_error(context.m_app, saved.error());
  }
  context.m_app.m_out << match->m_account->m_name << " is now the default account for "
                      << match->m_site << ".\n";
  context.m_app.m_out.flush();
  return code_of(exit_code::success);
}

int run_account_show(command_context& context, account_show_options const& options) {
  auto session = load_store(context);
  if (!session.has_value()) {
    return report_error(context.m_app, session.error());
  }
  auto sites = candidate_sites(context, session->m_file);
  if (!sites.has_value()) {
    return report_error(context.m_app, sites.error());
  }
  if (sites->empty()) {
    return report_error(context.m_app, missing_accounts_error("any site"));
  }

  auto match = resolve_one(context, session->m_file, *sites, options.m_selector);
  if (!match.has_value()) {
    return report_error(context.m_app, match.error());
  }
  auto const& record = *match->m_account;

  if (options.m_json) {
    write_json(context.m_app.m_out, account_json(session->m_file, record));
    return code_of(exit_code::success);
  }

  auto const* fallback = config::default_account(session->m_file, record.m_site);
  bool const is_default = fallback != nullptr && fallback->key() == record.key();
  auto const line = [&](std::string_view label, std::string value) {
    if (value.empty()) {
      value = dash;
    }
    context.m_app.m_out << label;
    context.m_app.m_out << std::string(12 - std::min<std::size_t>(12, label.size()), ' ') << value
                        << '\n';
  };
  line("site:", record.m_site);
  line("name:", record.m_name);
  line("vpc:", record.m_vpc);
  line("email:", record.m_email);
  line("uid:", record.m_uid);
  line("account_id:", record.m_account_id);
  line("kind:", kind_of(record));
  line("expires:", format_local_time(positive(record.m_expires_at)));
  line("default:", yes_no(is_default));
  context.m_app.m_out.flush();
  return code_of(exit_code::success);
}

namespace {

/// Confirms a deletion on a terminal; `--yes` and a non-terminal stdin skip the question.
bool confirmed(command_context& context, std::vector<account_candidate> const& targets,
               bool skip_question) {
  if (skip_question || !is_interactive(context.m_app)) {
    return true;
  }
  if (targets.size() == 1) {
    return confirm("Remove account '" + targets.front().m_account->m_name + "' for " +
                       targets.front().m_site + "?",
                   context.m_app);
  }
  std::string sites;
  for (auto const& target : targets) {
    if (sites.find(target.m_site) == std::string::npos) {
      if (!sites.empty()) {
        sites.append(", ");
      }
      sites.append(target.m_site);
    }
  }
  return confirm("Remove " + std::to_string(targets.size()) + " accounts for " + sites + "?",
                 context.m_app);
}

int apply_removal(command_context& context, store_session& session,
                  std::vector<account_candidate> const& targets, bool all, bool json_output) {
  std::map<std::string, std::vector<std::string>> per_site;
  for (auto const& target : targets) {
    per_site[target.m_site].push_back(target.m_account->m_name);
  }

  json removed = json::array();
  std::string first_site;
  for (auto& [site, names] : per_site) {
    if (first_site.empty()) {
      first_site = site;
    }
    auto batch = config::remove_accounts(session.m_file, site, names, all);
    if (!batch.has_value()) {
      return report_error(context.m_app, batch.error());
    }
    for (auto const& key : batch->m_removed_keys) {
      removed.push_back(key);
    }
  }

  auto saved = save_store(session);
  if (!saved.has_value()) {
    return report_error(context.m_app, saved.error());
  }

  if (json_output) {
    std::optional<std::string> fallback;
    if (!first_site.empty()) {
      if (auto const* account = config::default_account(session.m_file, first_site);
          account != nullptr) {
        fallback = account->key();
      }
    }
    write_json(context.m_app.m_out, json{{"removed", std::move(removed)}, {"default", fallback}});
    return code_of(exit_code::success);
  }

  for (auto const& key : removed) {
    context.m_app.m_out << "Removed " << key << ".\n";
  }
  context.m_app.m_out.flush();
  return code_of(exit_code::success);
}

/// Every account of `site`, which is what `--all` and a bare `logout` operate on.
std::vector<account_candidate> accounts_of_site(config::credential_file const& file,
                                                std::string_view site) {
  std::vector<account_candidate> targets;
  for (auto const* account : ordered_accounts(file, site)) {
    targets.push_back(account_candidate{std::string{site}, account->key(), account});
  }
  return targets;
}

} // namespace

int run_account_remove(command_context& context, remove_options const& options) {
  auto session = load_store(context);
  if (!session.has_value()) {
    return report_error(context.m_app, session.error());
  }

  if (options.m_all && !options.m_selectors.empty()) {
    return report_error(context.m_app, bad_request_error("--all and --account cannot be combined"));
  }

  if (options.m_all) {
    auto const site = resolve_site_definition(context.m_cli, context.m_env);
    if (!site.has_value()) {
      return report_error(context.m_app, site.error());
    }
    auto const targets = accounts_of_site(session->m_file, (*site)->m_id);
    if (targets.empty()) {
      return report_error(context.m_app, missing_accounts_error((*site)->m_id));
    }
    if (!confirmed(context, targets, options.m_yes)) {
      context.m_app.m_err << "aborted; nothing was removed\n";
      return code_of(exit_code::configuration);
    }
    return apply_removal(context, *session, targets, true, options.m_json);
  }

  auto sites = candidate_sites(context, session->m_file);
  if (!sites.has_value()) {
    return report_error(context.m_app, sites.error());
  }
  if (sites->empty()) {
    return report_error(context.m_app,
                        make_error(error_kind::auth, "not signed in; run: qoderz login"));
  }

  std::vector<account_candidate> targets;
  for (auto const& selector : options.m_selectors) {
    auto match = resolve_one(context, session->m_file, *sites, selector);
    if (!match.has_value()) {
      return report_error(context.m_app, match.error());
    }
    targets.push_back(std::move(*match));
  }
  if (targets.empty()) {
    return report_error(context.m_app,
                        bad_request_error("give at least one account selector, or use --all"));
  }
  if (!confirmed(context, targets, options.m_yes)) {
    context.m_app.m_err << "aborted; nothing was removed\n";
    return code_of(exit_code::configuration);
  }
  return apply_removal(context, *session, targets, false, options.m_json);
}

int run_logout(command_context& context, remove_options const& options) {
  if (options.m_all || !options.m_selectors.empty()) {
    return run_account_remove(context, options);
  }

  auto session = load_store(context);
  if (!session.has_value()) {
    return report_error(context.m_app, session.error());
  }

  auto const site = resolve_site_definition(context.m_cli, context.m_env);
  if (!site.has_value()) {
    return report_error(context.m_app, site.error());
  }
  auto const targets = accounts_of_site(session->m_file, (*site)->m_id);
  if (targets.empty()) {
    return report_error(context.m_app, missing_accounts_error((*site)->m_id));
  }
  if (targets.size() > 1) {
    std::string names;
    for (auto const& target : targets) {
      if (!names.empty()) {
        names.append(", ");
      }
      names.append(target.m_account->m_name);
    }
    return report_error(context.m_app,
                        configuration_error("several accounts for " + std::string{(*site)->m_id} +
                                            ": " + names + "; pass --account <name> or --all"));
  }
  if (!confirmed(context, targets, options.m_yes)) {
    context.m_app.m_err << "aborted; nothing was removed\n";
    return code_of(exit_code::configuration);
  }
  return apply_removal(context, *session, targets, true, options.m_json);
}

} // namespace qoderz::cli
