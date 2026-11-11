#pragma once

#include "support/stub_server.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace qoderz::test {

/// The single model the stub upstream advertises on `/algo/api/v2/model/list`.
inline constexpr std::string_view stub_model_key = "stub-model";
inline constexpr std::string_view stub_model_name = "Stub Model";

/// The shape of one fake chat generation: `m_chunks` content frames of `m_chunk_bytes` `d`
/// characters each, then a stop frame carrying usage, then `[DONE]`.
struct stub_upstream_options {
  std::size_t m_chunks = 8;
  std::size_t m_chunk_bytes = 256;
};

/// One routed request: the answer plus whether the route was known.
///
/// An unknown path answers `200 {}` and sets `m_handled` to false, which is how the executable
/// reports the request on stderr without turning a stray call into a failure.
struct stub_route_result {
  stub_response m_response;
  bool m_handled = true;
};

/// The path part of a request target, without the query string.
[[nodiscard]] std::string_view stub_path_of(std::string_view target) noexcept;

/// The `/algo/api/v2/model/list` body: one enabled model with a 1,000,000 token window and the
/// low/medium/high effort ladder, defaulting to medium.
[[nodiscard]] std::string stub_model_listing_body();

/// `m_chunks * m_chunk_bytes`: how many `d` characters one chat generation produces.
[[nodiscard]] std::size_t stub_completion_length(stub_upstream_options const& options) noexcept;

/// The whole SSE body of one chat generation, with every frame terminated by a blank line.
[[nodiscard]] std::string stub_chat_stream_body(stub_upstream_options const& options);

/// The job token refresh answer; both tokens are obviously fake and the expiry is far away.
[[nodiscard]] std::string stub_job_token_body();

/// The `/sash/api/v2/me/usage` answer.
[[nodiscard]] std::string stub_usage_body();

/// Routes one upstream request by path only; the token, the headers and the body are never read.
///
/// This is the one place that knows the protocol fixture, so the `qoderz_stub_upstream` executable
/// and the tests that assert on the same shapes cannot drift apart.
[[nodiscard]] stub_route_result stub_route(stub_request const& request,
                                           stub_upstream_options const& options);

} // namespace qoderz::test
