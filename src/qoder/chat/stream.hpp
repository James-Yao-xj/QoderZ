#pragma once

#include "common/error.hpp"
#include "common/json.hpp"
#include "common/net/http_types.hpp"
#include "protocol/sse/chat_event.hpp"
#include "protocol/sse/line_reader.hpp"
#include "protocol/sse/text_tool_splitter.hpp"
#include "qoder/chat/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::qoder::chat {

/// Parses upstream SSE lines into events, following `docs/protocol/qoder-chat.md` §6–§8.
///
/// A line that is not an envelope, an envelope whose body is not the inner chunk, and an inner
/// chunk without choices all yield nothing: a damaged frame never aborts a response that is
/// otherwise readable. Only a failure envelope, an unreadable text tool call and an unnamed native
/// tool call at the end produce an `error` event, and those end the stream without a `stop`.
///
/// The text splitter numbers the calls it finds; those indices are renumbered into the same counter
/// the native `tool_calls` use, so downstream sees one increasing sequence whichever way the model
/// wrote the call. Not thread-safe.
class stream_parser {
public:
  /// Feeds one SSE line, with or without its line terminator.
  std::vector<protocol::chat_event> on_line(std::string_view line);

  /// Ends the response at `[DONE]` or EOF: flushes the text splitter, then emits the single `stop`
  /// or the `error` that replaces it. Calling it again returns nothing.
  std::vector<protocol::chat_event> finish();

  [[nodiscard]] bool done() const noexcept { return m_done; }

private:
  struct tool_state {
    std::string m_id;
    std::string m_name;
    std::string m_arguments;
    int m_output_index = -1;
    bool m_started = false;
  };

  /// Appends `produced` after renumbering its tool calls; false when one of them is an error, which
  /// ends the stream as it stands.
  bool append(std::vector<protocol::chat_event>& events,
              std::vector<protocol::chat_event> produced);

  void apply_tool_call(int upstream_index, std::string id, std::string name, std::string arguments,
                       std::vector<protocol::chat_event>& events, bool& failed);

  void record_usage(json const& chunk);
  void record_finish_reason(json const& choice);

  std::vector<protocol::chat_event> apply_envelope(std::string_view payload);
  std::vector<protocol::chat_event> apply_body_string(std::string const& text);
  std::vector<protocol::chat_event> apply_chunk(json const& chunk);
  std::vector<protocol::chat_event> complete();
  std::vector<protocol::chat_event> failure_event(int status, std::string const& text);

  protocol::text_tool_splitter m_splitter;
  std::map<int, tool_state> m_tools;
  std::optional<protocol::token_usage> m_usage;
  std::string m_finish_reason;
  int m_next_tool_index = 0;
  bool m_saw_tool = false;
  bool m_done = false;
};

/// A `chat_stream` over one HTTP response body.
///
/// Lines come from `protocol::sse_line_reader` and events from `stream_parser`; a parsed batch is
/// queued so a frame carrying several events is delivered whole and in order. The body is owned, so
/// destroying this stream ends the response.
class sse_chat_stream final : public chat_stream {
public:
  explicit sse_chat_stream(std::unique_ptr<net::http_stream> body);

  /// Puts an already parsed event back in front of the queue. The client reads the first event of a
  /// response before it can decide whether the account may still be swapped, and hands that event
  /// to the stream it finally returns so nothing is lost or reordered.
  void requeue(protocol::chat_event event);

  boost::asio::awaitable<result<std::optional<protocol::chat_event>>> next() override;

private:
  std::unique_ptr<net::http_stream> m_body;
  protocol::sse_line_reader m_reader;
  stream_parser m_parser;
  std::deque<protocol::chat_event> m_pending;
  bool m_eof = false;
  bool m_finished = false;
};

} // namespace qoderz::qoder::chat
