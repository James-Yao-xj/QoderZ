#pragma once

#include "common/error.hpp"

#include <string>
#include <string_view>

namespace qoderz::net {

/// Components of an absolute `http`/`https` URL.
///
/// The port is always filled in and the host is lowercased so that two spellings of one host share
/// a single pool bucket. IPv6 literals are stored without brackets and get them back in
/// `origin_of`.
struct parsed_url {
  std::string m_scheme; // http | https
  std::string m_host;
  std::string m_port;
  std::string m_target; // path + query, at least "/"
  bool m_tls = false;
};

/// Parses an absolute URL, accepting only `http` and `https`.
///
/// Relative references, userinfo, fragments, an empty host and an out-of-range port are rejected
/// rather than silently dropped: a dropped `#fragment` or `user:pass@` would change where the
/// request lands. The query is kept verbatim and an empty path becomes `/`. Failures are
/// `error_kind::bad_request` and never echo the URL, whose query may carry credentials.
[[nodiscard]] result<parsed_url> parse_url(std::string_view url);

/// True for `localhost` (any case), any address in `127.0.0.0/8` and `::1`.
///
/// Purely textual — no DNS lookup, no socket — so it is safe to call from any thread.
[[nodiscard]] bool is_loopback_host(std::string_view host) noexcept;

/// `scheme://host:port` with brackets restored for IPv6 literals; the pool key.
[[nodiscard]] std::string origin_of(parsed_url const& url);

} // namespace qoderz::net
