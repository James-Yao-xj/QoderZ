#pragma once

#include "common/error.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::qoder::auth {

/// A response kept whole: the status, the raw text (for the error mapper) and the parsed body when
/// it happened to be JSON. A non-JSON body is not a failure by itself — the status still decides.
struct json_exchange {
  int m_status = 0;
  json m_body;
  std::string m_text;
};

inline constexpr std::chrono::milliseconds default_expiry{std::chrono::hours{24}};

[[nodiscard]] net::http_request json_post(std::string url, json const& body);

/// A GET that expects JSON and carries a bearer token; an empty token adds no `Authorization`.
[[nodiscard]] net::http_request bearer_get(std::string url, std::string_view token);

void set_bearer(net::http_request& request, std::string_view token);

/// Sends the request and parses whatever body came back. Transport failures keep their upstream
/// classification; 4xx and 5xx come back as a value for the caller to classify.
[[nodiscard]] boost::asio::awaitable<result<json_exchange>> send_json(net::http_client& http,
                                                                      net::http_request request);

/// The classified error of a failed exchange, via the protocol table.
[[nodiscard]] error explain(json_exchange const& exchange);

/// The first of `names` whose value is a JSON string, or an empty string.
[[nodiscard]] std::string read_text(json const& object,
                                    std::initializer_list<std::string_view> names);

/// The first of `names` whose value is a number or a numeric string, or `fallback`.
[[nodiscard]] std::int64_t read_int(json const& object,
                                    std::initializer_list<std::string_view> names,
                                    std::int64_t fallback = 0);

/// ISO-8601 to Unix milliseconds; `nullopt` for anything else.
[[nodiscard]] std::optional<std::int64_t> parse_timestamp_ms(std::string_view text);

/// When the token of this response expires, in Unix milliseconds.
///
/// An absolute field wins; a bare number below 1e12 is seconds rather than milliseconds. Otherwise
/// `expires_in` is added to `now_ms` in `expires_in_unit`, because the job endpoints count
/// milliseconds while the device endpoints count seconds. Neither present means one day out.
[[nodiscard]] std::int64_t read_expires_at(json const& object, std::int64_t now_ms,
                                           std::chrono::milliseconds expires_in_unit,
                                           std::chrono::milliseconds fallback = default_expiry);

/// A random version 4 UUID in the 36-character lowercase form the device flow expects.
[[nodiscard]] std::string make_uuid();

} // namespace qoderz::qoder::auth
