#include "server/error_writer.hpp"

#include "common/json.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace qoderz::server {
namespace {

constexpr std::string_view default_message = "The request failed.";
constexpr std::string_view cancelled_message = "The request was cancelled.";
constexpr std::string_view unexpected_message = "An unexpected error occurred.";

constexpr int status_of(error_kind kind) noexcept {
  switch (kind) {
  case error_kind::bad_request:
    return 400;
  case error_kind::configuration:
    return 500;
  case error_kind::auth:
    return 401;
  case error_kind::quota:
    return 429;
  case error_kind::busy:
    return 503;
  case error_kind::upstream:
    return 502;
  case error_kind::io:
    // Local I/O failure, not an upstream one: the shared mapping in common/error.hpp owns this
    // number, so the writer follows it instead of inventing a gateway-specific status.
    return 500;
  case error_kind::protocol:
    return 502;
  case error_kind::canceled:
    return 499;
  case error_kind::internal:
    return 500;
  }
  return 500;
}

/// Only `bad_request` honours a pinned status: the router pins 404/405/413 on that kind, while an
/// auth or quota error must keep its own status no matter what an upstream said.
constexpr bool is_client_status(int status) noexcept {
  return status >= 400 && status <= 499;
}

std::string_view fallback_message(error_kind kind) noexcept {
  switch (kind) {
  case error_kind::canceled:
    return cancelled_message;
  case error_kind::internal:
    return unexpected_message;
  default:
    return default_message;
  }
}

} // namespace

int http_status_for(error const& value) noexcept {
  if (value.m_kind == error_kind::bad_request && is_client_status(value.m_http_status)) {
    return value.m_http_status;
  }
  return status_of(value.m_kind);
}

std::string type_for(error const& value) {
  switch (value.m_kind) {
  case error_kind::bad_request:
    return "invalid_request_error";
  case error_kind::configuration:
    return "configuration_error";
  case error_kind::auth:
    return "authentication_error";
  case error_kind::quota:
    return "insufficient_quota";
  case error_kind::busy:
    return "server_busy";
  case error_kind::upstream:
    return "upstream_error";
  case error_kind::io:
    return "internal_error";
  case error_kind::protocol:
    return "upstream_error";
  case error_kind::canceled:
    return "cancelled";
  case error_kind::internal:
    return "internal_error";
  }
  return "internal_error";
}

std::string to_json(error const& value) {
  auto message = value.m_message;
  if (message.empty()) {
    message = std::string{fallback_message(value.m_kind)};
  }
  json const body = {
      {"error",
       {{"message", message}, {"type", type_for(value)}, {"code", http_status_for(value)}}}};
  return body.dump();
}

std::optional<int> retry_after_for(error const& value) noexcept {
  if (value.m_kind != error_kind::busy || !value.m_retry_after.has_value()) {
    return std::nullopt;
  }
  if (*value.m_retry_after <= 0) {
    return std::nullopt;
  }
  return value.m_retry_after;
}

std::string to_sse_frame(error const& value) {
  std::string frame = "data: ";
  frame.append(to_json(value));
  frame.append("\n\n");
  return frame;
}

error client_status_error(int status, std::string message) {
  error value = make_error(error_kind::bad_request, std::move(message));
  value.m_http_status = status;
  return value;
}
} // namespace qoderz::server
