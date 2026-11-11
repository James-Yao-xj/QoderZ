#pragma once

#include "cli/app.hpp"
#include "cli/commands/models.hpp"
#include "common/error.hpp"

namespace qoderz::cli {

/// `qoderz usage`: one snapshot of the selected account's plan and quota windows.
[[nodiscard]] int run_usage(command_context& context, query_options const& options);

} // namespace qoderz::cli
