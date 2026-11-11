#pragma once

#include "cli/app.hpp"
#include "cli/options.hpp"
#include "common/error.hpp"
#include "config/site.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace qoderz::cli {

/// `qoderz models` and `qoderz usage` of `docs/design/cli.md` §6. Both select one account the same
/// way and resolve the same loopback override, so they share the selection helper.
struct query_options {
  std::string m_account;
  bool m_json = false;
};

/// One account plus everything whose token and upstream call it needs.
struct query_target {
  config::account_record m_account;
  config::resolved_site m_site;
  std::filesystem::path m_store_path;
  config::site_definition m_site_definition;
  std::optional<std::string> m_upstream;
};

[[nodiscard]] result<query_target> prepare_query(command_context& context,
                                                 query_options const& options);

[[nodiscard]] int run_models(command_context& context, query_options const& options);

} // namespace qoderz::cli
