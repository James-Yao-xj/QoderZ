#pragma once

#include "common/error.hpp"
#include "common/net/url.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace qoderz::server {

/// Everything the HTTP listener needs, per docs/design/server.md §1.
///
/// The CLI owns parsing and defaults; this struct is the finished decision, so the server never
/// looks at the environment or the command line itself.
struct server_config {
  std::string m_listen_host = "127.0.0.1";
  std::uint16_t m_port = 8788;
  std::string m_site_id = "qoder-cn";
  std::optional<std::string> m_api_key;
  std::size_t m_max_body_bytes = 32 * 1024 * 1024;
  bool m_allow_public = false;
};

/// Limits of the listener itself. Only the body limit is user visible (`--max-body-bytes`); the
/// rest are fixed by docs/design/server.md §1 and §7.
inline constexpr std::size_t max_header_bytes = 16 * 1024;
inline constexpr std::chrono::seconds request_timeout{30};
inline constexpr std::chrono::seconds response_timeout{60};
inline constexpr std::chrono::seconds shutdown_grace{5};

/// The startup safety check of docs/design/server.md §1 and §7.
///
/// A listener that is not loopback must carry an API key unless the operator explicitly asked for a
/// public listener. The check is textual on purpose: resolving the host first would let a DNS name
/// that answers with 127.0.0.1 today pass, while its records change later.
[[nodiscard]] inline result<void> validate_config(server_config const& config) {
  if (net::is_loopback_host(config.m_listen_host)) {
    return {};
  }
  if (config.m_api_key.has_value() && !config.m_api_key->empty()) {
    return {};
  }
  if (config.m_allow_public) {
    return {};
  }
  return std::unexpected(
      configuration_error("refusing to listen on '" + config.m_listen_host +
                          "' without an API key; set --api-key, QODERZ_API_KEY or --allow-public"));
}

} // namespace qoderz::server
