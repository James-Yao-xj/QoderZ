#pragma once

#include "common/error.hpp"

#include <string>
#include <string_view>

namespace qoderz::server {

/// The endpoints of docs/design/server.md §2, plus the two answers that are not endpoints.
enum class route_kind { health, models, chat, wrong_method, not_found };

struct route_match {
  route_kind m_kind = route_kind::not_found;
  /// The `Allow` value of a `wrong_method` match: `GET` or `POST`. Empty otherwise.
  std::string m_allow{};
};

inline constexpr std::string_view unknown_endpoint_message =
    "unknown endpoint; use /v1/models or /v1/chat/completions";

/// The path part of a request target, without the query string.
[[nodiscard]] std::string_view path_of(std::string_view target) noexcept;

/// Whether the endpoint a target names sits behind the API key of §1.
///
/// `/healthz` and unknown paths are public; both `/v1` endpoints are not, and that includes the 405
/// answer for a wrong method on them.
[[nodiscard]] bool requires_authentication(std::string_view target) noexcept;

/// Matches one request against the fixed route table.
///
/// `target` is the raw request target: the query is dropped and the path is compared exactly, so
/// `/v1/models/` does not match. A known path with another method is `wrong_method` — QoderZ's
/// improvement over the reference implementation, which answers 404 there.
[[nodiscard]] route_match match_route(std::string_view method, std::string_view target);

/// 404 with the fixed wording of docs/design/server.md §2; never authenticated.
[[nodiscard]] error not_found_error();

/// 405 carrying the method the endpoint accepts.
[[nodiscard]] error wrong_method_error(std::string_view allow);

} // namespace qoderz::server
