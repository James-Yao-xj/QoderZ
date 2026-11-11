#pragma once

#include <string>

namespace qoderz::server::endpoints {

/// The fixed `/healthz` body of docs/design/server.md §2.
[[nodiscard]] std::string health_json();

} // namespace qoderz::server::endpoints
