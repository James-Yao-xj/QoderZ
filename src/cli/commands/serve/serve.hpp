#pragma once

#include "cli/app.hpp"
#include "cli/commands/serve/wiring.hpp"
#include "common/error.hpp"

namespace qoderz::cli {

/// `qoderz serve`: plans, wires and runs the listener of `docs/design/cli.md` §7.
[[nodiscard]] int run_serve(command_context& context, serve_options const& options);

} // namespace qoderz::cli
