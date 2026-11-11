#pragma once

#include "common/error.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace qoderz::server {

/// The HTTP status of docs/design/server.md §3.
///
/// The table is keyed by `error_kind` rather than by `error::m_http_status`, so the facade keeps a
/// stable status per category and an upstream 500 never surfaces as 500 instead of 502. The one
/// exception is a more specific *client* status, which the router needs for 404, 405 and 413: a 4xx
/// override on a 4xx category is the caller pinning the answer, not an upstream value leaking.
[[nodiscard]] int http_status_for(error const& value) noexcept;

/// The OpenAI `type` of docs/design/server.md §3.
[[nodiscard]] std::string type_for(error const& value);

/// `{"error":{"message","type","code"}}` with `code` as the numeric status.
///
/// An empty message falls back to a per-category sentence, so a client never sees an empty string.
[[nodiscard]] std::string to_json(error const& value);

/// `Retry-After` in seconds: only a busy error with a positive hint carries one.
[[nodiscard]] std::optional<int> retry_after_for(error const& value) noexcept;

/// One SSE error frame, newline terminated twice.
[[nodiscard]] std::string to_sse_frame(error const& value);

/// A `bad_request` error pinned to a more specific client status (404, 405, 413).
///
/// The kind stays `bad_request` so the type is `invalid_request_error`; a non-4xx status is ignored
/// and the category status wins. The body carries only the numeric status, so there is no code
/// argument to pass: §3 of docs/design/server.md keeps Qoder codes out of `code`.
[[nodiscard]] error client_status_error(int status, std::string message);

} // namespace qoderz::server
