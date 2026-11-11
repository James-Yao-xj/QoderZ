#pragma once

#include "common/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <string>
#include <string_view>

namespace qoderz::server {

/// How a chat handler writes its answer, per docs/design/server.md §5.3 and §5.4.
///
/// One sink belongs to one request on one connection and is driven by that connection's coroutine,
/// so frames leave in the order they are asked for and no lock is needed.
///
/// Once `started()` is true the status line is already on the wire, so a later error can only be
/// reported as an SSE error frame — never as a 4xx/5xx status.
class response_sink {
public:
  response_sink() = default;
  response_sink(response_sink const&) = delete;
  response_sink(response_sink&&) = delete;
  response_sink& operator=(response_sink const&) = delete;
  response_sink& operator=(response_sink&&) = delete;
  virtual ~response_sink() = default;

  [[nodiscard]] virtual bool started() const noexcept = 0;

  /// Whether the connection may carry another request after this sink. A complete JSON answer says
  /// yes when the request itself allowed keep-alive; an SSE answer always says no, because the
  /// head announces `Connection: close` and a client that stopped reading mid-stream must not leave
  /// a half-consumed socket behind. The answer stays false once `begin_sse()` has been called, even
  /// after `end_sse()` has written the terminating chunk.
  ///
  /// The default refuses reuse, so a sink that does not model the connection keeps the safe answer.
  [[nodiscard]] virtual bool reusable() const noexcept { return false; }

  /// Writes a complete `application/json` response. Only valid before `started()`.
  virtual boost::asio::awaitable<result<void>> write_json(int status, std::string body) = 0;

  /// Writes the chunked `text/event-stream` head and switches to streaming mode.
  virtual boost::asio::awaitable<result<void>> begin_sse() = 0;

  /// Writes one complete frame, which the caller has already framed (`data: ...\n\n`).
  virtual boost::asio::awaitable<result<void>> write_sse(std::string_view frame) = 0;

  /// Writes the terminating chunk. The connection is not reusable afterwards, which `reusable()`
  /// reports: the router asks it instead of remembering the rule.
  virtual boost::asio::awaitable<result<void>> end_sse() = 0;
};

} // namespace qoderz::server