#pragma once

#include "openai/response/response.hpp"
#include "protocol/sse/chat_event.hpp"

#include <string>
#include <vector>

namespace qoderz::openai {

/// Turns the event stream of one answer into OpenAI SSE frames, per
/// `docs/protocol/openai-compat.md` §8.
///
/// Every frame is a whole `data: ...\n\n` block, so the caller writes what it is given without
/// adding separators. A role frame precedes the first non-error event whatever it is; an `error`
/// event closes the stream without `[DONE]` and without a `finish_reason`. The state machine is
/// terminal after a `stop` or an `error`, and every method is a no-op then.
class stream_translator {
public:
  explicit stream_translator(response_identity identity);

  /// The frames this event produces, in order; empty once the stream is terminal.
  std::vector<std::string> on_event(protocol::chat_event const& event);

  /// The `[DONE]` frame for a stream that ended without a `stop`; empty when terminal.
  std::string finish();

  [[nodiscard]] bool answered() const noexcept;
  [[nodiscard]] bool failed() const noexcept;

private:
  void append_role_frame(std::vector<std::string>& frames);

  [[nodiscard]] std::string frame(nlohmann::ordered_json const& chunk) const;
  [[nodiscard]] nlohmann::ordered_json chunk_with(nlohmann::ordered_json delta,
                                                  nlohmann::ordered_json finish_reason) const;
  [[nodiscard]] std::string error_frame(protocol::chat_event const& event) const;

  response_identity m_identity;
  bool m_role_sent = false;
  bool m_terminal = false;
  bool m_answered = false;
  bool m_failed = false;
};

} // namespace qoderz::openai
