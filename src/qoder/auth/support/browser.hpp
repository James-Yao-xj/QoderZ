#pragma once

#include "common/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace qoderz::qoder::auth {

/// The argv a platform uses to hand a URL to the default browser, split out so a test can assert it
/// without starting anything. Windows inserts the empty title argument that `start` otherwise takes
/// to be the URL.
[[nodiscard]] std::vector<std::string> browser_command(std::string_view url);

/// Launches the browser and waits for the launcher to exit.
///
/// The wait is deliberate: `xdg-open` and `open` hand the URL to a browser that outlives them and
/// exit shortly after, so the wait costs one process spawn and buys a real exit status — an exec
/// that never happened would otherwise be indistinguishable from a browser that opened.
/// A launch failure — no binary, no display, a refused fork — is an `io` error. It is deliberately
/// not fatal: the CLI prints the URL, so the user can always open it by hand.
[[nodiscard]] result<void> open_browser(std::string_view url);

} // namespace qoderz::qoder::auth
