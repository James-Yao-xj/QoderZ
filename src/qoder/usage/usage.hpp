#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qoderz::qoder::usage {

/// One quota window of the usage reply.
///
/// `m_used` and `m_total` are the raw amounts so a renderer can choose its own precision;
/// `m_percent` and `m_display` are precomputed because they are the two values every caller shows,
/// and the rounding rules are part of the protocol rather than a presentation detail.
struct usage_window {
  std::string m_name;
  std::string m_unit;
  double m_used = 0;
  double m_total = 0;
  int m_percent = 0;
  std::string m_display;
};

/// What one account may spend right now. `m_plan` is absent when the reply names no plan, and
/// `m_until_ms` is Unix milliseconds, absent when the reply names no expiry.
struct usage_snapshot {
  std::optional<std::string> m_plan;
  std::optional<std::int64_t> m_until_ms;
  std::vector<usage_window> m_windows;
};

/// Reads `{openapi}/sash/api/v2/me/usage` for one account.
///
/// The service holds no cache: the quota changes whenever the account spends, so a stale snapshot
/// would be actively misleading. It only parses and classifies; rendering belongs to the CLI.
class usage_service {
public:
  usage_service(net::http_client& http, clock& time);

  /// The snapshot of `account`.
  ///
  /// The bearer token follows the site's own rule: a PAT account without a device token uses the
  /// job access token, everything else prefers the device token. A non-200 answer is classified by
  /// the protocol table; a 200 whose body carries no quota block is an upstream error rather than
  /// an empty snapshot, because "no data" and "zero credits" must not look alike.
  [[nodiscard]] boost::asio::awaitable<result<usage_snapshot>>
  fetch(config::resolved_site const& site, config::account_record const& account);

private:
  net::http_client& m_http;
  clock& m_time;
};

/// The token the usage endpoint expects for `account`, as `docs/protocol/qoder-auth.md` §7 states.
[[nodiscard]] std::string select_token(config::account_record const& account);

/// Parses a usage reply body. Exposed for tests and for callers that already hold the text.
[[nodiscard]] result<usage_snapshot> parse_usage(std::string_view body);

} // namespace qoderz::qoder::usage
