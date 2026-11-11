#pragma once

#include "common/error.hpp"
#include "common/json.hpp"
#include "protocol/sse/chat_event.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace qoderz::openai {

struct response_identity {
  std::string m_id;
  std::int64_t m_created = 0;
  std::string m_model;
};

struct completion_result {
  std::string m_json;
  /// False when the run ended without a `stop` event: the document is complete but unanswered.
  bool m_answered = false;
};

/// `chatcmpl-` followed by 32 lowercase hex characters.
[[nodiscard]] std::string make_completion_id();

/// Merge one answer into a `chat.completion` document, per `docs/protocol/openai-compat.md` §7.
///
/// An `error` event discards everything collected before it and becomes the returned error, so a
/// failed run never yields a partial body. Events are expected in protocol order.
[[nodiscard]] result<completion_result>
build_completion(std::span<protocol::chat_event const> events, response_identity const& identity);

namespace detail {

struct collected_tool_call {
  int m_index = 0;
  std::string m_id;
  std::string m_name;
  std::string m_arguments;
};

struct completion_state {
  std::string m_content;
  std::string m_reasoning;
  std::vector<collected_tool_call> m_tools;
  std::string m_reason;
  std::optional<protocol::token_usage> m_usage;
  bool m_stopped = false;
};

/// Classifies an `error` event for the client. The event's own message wins, so an unclassified
/// status cannot replace a real reason with the status placeholder; `from_status` still decides
/// the kind and the HTTP status, which both response shapes report.
[[nodiscard]] error error_of(protocol::chat_event const& event);

[[nodiscard]] std::string choose_finish_reason(completion_state const& state);

/// Shared by both response shapes so the token field order cannot drift between them.
[[nodiscard]] nlohmann::ordered_json make_usage(protocol::token_usage const& usage);

} // namespace detail

} // namespace qoderz::openai
