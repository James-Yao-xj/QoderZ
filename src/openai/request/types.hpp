#pragma once

#include "qoder/catalog/model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qoderz::openai {

struct tool_call {
  std::string m_id;
  std::string m_name;
  /// The client's JSON text, re-emitted verbatim: its reader expects the same shape back.
  std::string m_arguments;
};

struct message_block {
  enum class kind { text, image_url };

  kind m_kind = kind::text;
  std::string m_text;
  std::string m_url;
  /// Marks the single block the upstream prompt cache may reuse; at most one block carries it.
  bool m_cache_control = false;
};

struct normalized_message {
  /// One of `system`, `user`, `assistant`, `tool`; the Qoder body accepts nothing else.
  std::string m_role;
  std::vector<message_block> m_blocks;
  /// The plain content of a `tool` result; `m_blocks` holds the content of every other role.
  std::string m_text;
  std::string m_tool_call_id;
  std::vector<tool_call> m_tool_calls;
  /// An array-shaped client `content`, which has to be written twice (`content` and `contents`).
  bool m_array_content = false;
};

struct tool_spec {
  std::string m_name;
  std::string m_description;
  /// Embedded as-is, so schema fields this layer does not model survive the round trip.
  std::string m_parameters_json;
};

struct chat_task {
  std::string m_id;
  std::int64_t m_begin_at_ms = 0;
  bool m_answered = false;
  std::string m_name;
};

struct request_context {
  qoder::catalog::model_info m_model;
  std::string m_system_text;
  std::vector<normalized_message> m_messages;
  std::vector<tool_spec> m_tools;
  bool m_include_tools = false;
  bool m_enable_thinking = true;
  std::string m_reasoning_effort;
  int m_max_tokens = 32000;
  std::string m_session_id;
  chat_task m_task;
  std::string m_request_id;
};

} // namespace qoderz::openai
