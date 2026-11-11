#pragma once

#include "cli/app.hpp"
#include "cli/options.hpp"
#include "common/error.hpp"

#include <optional>
#include <string>

namespace qoderz::cli {

/// `qoderz login` of `docs/design/cli.md` §2. An empty string means the flag was not given, which
/// is the same rule the environment follows.
struct login_options {
  std::string m_name;
  std::string m_vpc;
  std::string m_pat;
  bool m_pat_stdin = false;
  bool m_device = false;
  bool m_no_browser = false;
  bool m_replace = false;
  bool m_json = false;
};

/// Resolves the PAT from `--pat`, `--pat-stdin` or `QODERZ_PAT`, rejecting two sources at once.
/// The stdin form reads one line and trims it, so the secret never reaches argv.
[[nodiscard]] result<std::optional<std::string>>
resolve_pat(login_options const& options, env_options const& env, app_context& app);

/// Signs in, stores the account and prints the result. `--name` is validated before any network
/// access; the JSON form never carries a token.
[[nodiscard]] int run_login(command_context& context, login_options const& options);

} // namespace qoderz::cli
