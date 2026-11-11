#pragma once

#include "cli/app.hpp"
#include "cli/options.hpp"
#include "common/error.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qoderz::cli {

/// `qoderz accounts` of `docs/design/cli.md` §3. Never touches the network.
struct accounts_options {
  bool m_all_sites = false;
  bool m_json = false;
  bool m_wide = false;
};

[[nodiscard]] int run_accounts(command_context& context, accounts_options const& options);

struct account_use_options {
  std::string m_selector;
};

[[nodiscard]] int run_account_use(command_context& context, account_use_options const& options);

struct account_show_options {
  std::string m_selector;
  bool m_json = false;
};

[[nodiscard]] int run_account_show(command_context& context, account_show_options const& options);

/// `qoderz account remove` and `qoderz logout` share this behaviour, per §4.
struct remove_options {
  std::vector<std::string> m_selectors;
  bool m_all = false;
  bool m_yes = false;
  bool m_json = false;
};

[[nodiscard]] int run_account_remove(command_context& context, remove_options const& options);

/// `qoderz logout`: with no selector a site holding one account removes it, and a site holding
/// several fails with the names listed.
[[nodiscard]] int run_logout(command_context& context, remove_options const& options);

} // namespace qoderz::cli
