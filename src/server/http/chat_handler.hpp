#pragma once

#include "common/error.hpp"
#include "server/http/response_sink.hpp"

#include <boost/asio/awaitable.hpp>
#include <string>

namespace qoderz::server {

/// One chat request as the router hands it over.
///
/// Parsing and validating the JSON body is the handler's job, because only it knows which fields it
/// needs; the router only enforces the size limit and extracts the session header.
struct chat_http_request {
  std::string m_body;
  std::string m_session_id;
  bool m_stream = false;
};

/// The pluggable chat orchestration of docs/design/server.md §5.
///
/// A handler writes through `response_sink` and returns an error only when it could not answer.
/// Before the sink starts, the router turns that error into an HTTP status; after, the router
/// writes one SSE error frame and closes the connection.
class chat_handler {
public:
  chat_handler() = default;
  chat_handler(chat_handler const&) = delete;
  chat_handler(chat_handler&&) = delete;
  chat_handler& operator=(chat_handler const&) = delete;
  chat_handler& operator=(chat_handler&&) = delete;
  virtual ~chat_handler() = default;

  virtual boost::asio::awaitable<result<void>> handle(chat_http_request const& request,
                                                      response_sink& sink) = 0;
};

} // namespace qoderz::server