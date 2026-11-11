#pragma once

#include "common/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::net {

struct http_header {
  std::string m_name;
  std::string m_value;
};

struct http_request {
  std::string m_method = "GET";
  std::string m_url;
  std::vector<http_header> m_headers;
  std::string m_body;
};

struct http_response {
  int m_status = 0;
  std::vector<http_header> m_headers;
  std::string m_body;
};

/// Case-insensitive lookup, first match wins. Returns nullptr when the field is absent.
[[nodiscard]] std::string const* header_value(std::vector<http_header> const& headers,
                                              std::string_view name) noexcept;

/// A response body whose headers are already parsed.
///
/// `read_some` hands out the body as the transport delivers it, with chunked framing already
/// removed. The stream owns its connection: destroying it mid-body closes that connection instead
/// of returning a half-read socket to the pool, and reaching the end of a keep-alive body returns
/// it. Implementations are not thread-safe; drive one stream from one coroutine at a time.
class http_stream {
public:
  http_stream() = default;
  http_stream(http_stream const&) = delete;
  http_stream& operator=(http_stream const&) = delete;
  virtual ~http_stream() = default;

  [[nodiscard]] virtual int status() const noexcept = 0;

  [[nodiscard]] virtual std::vector<http_header> const& headers() const noexcept = 0;

  /// Next body chunk: `nullopt` at a clean end of message, `error` on transport, parse or timeout
  /// failure. An empty string with a value is not produced.
  virtual boost::asio::awaitable<result<std::optional<std::string>>> read_some() = 0;
};

} // namespace qoderz::net
