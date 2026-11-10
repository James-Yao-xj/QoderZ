#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz {

enum class error_kind {
  configuration,
  auth,
  quota,
  busy,
  bad_request,
  upstream,
  io,
  protocol,
  canceled,
  internal,
};

struct error {
  error_kind m_kind;
  std::string m_message;
  int m_http_status = 0;
  std::string m_code;
  std::optional<int> m_retry_after;
};

template <class value_type> using result = std::expected<value_type, error>;

[[nodiscard]] constexpr std::string_view to_string(error_kind kind) noexcept;

[[nodiscard]] constexpr int default_http_status(error_kind kind) noexcept;

[[nodiscard]] error make_error(error_kind kind, std::string message, std::string code = {});

[[nodiscard]] error configuration_error(std::string message, std::string code = {});

[[nodiscard]] error auth_error(std::string message, std::string code = {});

[[nodiscard]] error quota_error(std::string message, std::string code = {},
                                std::optional<int> retry_after = std::nullopt);

[[nodiscard]] error bad_request_error(std::string message, std::string code = {});

[[nodiscard]] error upstream_error(std::string message, std::string code = {});

[[nodiscard]] error internal_error(std::string message, std::string code = {});

constexpr std::string_view to_string(error_kind kind) noexcept {
  switch (kind) {
  case error_kind::configuration:
    return "configuration";
  case error_kind::auth:
    return "auth";
  case error_kind::quota:
    return "quota";
  case error_kind::busy:
    return "busy";
  case error_kind::bad_request:
    return "bad_request";
  case error_kind::upstream:
    return "upstream";
  case error_kind::io:
    return "io";
  case error_kind::protocol:
    return "protocol";
  case error_kind::canceled:
    return "canceled";
  case error_kind::internal:
    return "internal";
  }
  return "unknown";
}

constexpr int default_http_status(error_kind kind) noexcept {
  switch (kind) {
  case error_kind::configuration:
    return 500;
  case error_kind::auth:
    return 401;
  case error_kind::quota:
    return 429;
  case error_kind::busy:
    return 503;
  case error_kind::bad_request:
    return 400;
  case error_kind::upstream:
    return 502;
  case error_kind::io:
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

inline error make_error(error_kind kind, std::string message, std::string code) {
  return error{
      .m_kind = kind,
      .m_message = std::move(message),
      .m_http_status = default_http_status(kind),
      .m_code = std::move(code),
      .m_retry_after = std::nullopt,
  };
}

inline error configuration_error(std::string message, std::string code) {
  return make_error(error_kind::configuration, std::move(message), std::move(code));
}

inline error auth_error(std::string message, std::string code) {
  return make_error(error_kind::auth, std::move(message), std::move(code));
}

inline error quota_error(std::string message, std::string code, std::optional<int> retry_after) {
  error err = make_error(error_kind::quota, std::move(message), std::move(code));
  err.m_retry_after = retry_after;
  return err;
}

inline error bad_request_error(std::string message, std::string code) {
  return make_error(error_kind::bad_request, std::move(message), std::move(code));
}

inline error upstream_error(std::string message, std::string code) {
  return make_error(error_kind::upstream, std::move(message), std::move(code));
}

inline error internal_error(std::string message, std::string code) {
  return make_error(error_kind::internal, std::move(message), std::move(code));
}

} // namespace qoderz
