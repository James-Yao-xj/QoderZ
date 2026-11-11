#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace qoderz::protocol {

enum class chat_event_kind { text, reasoning, tool_start, tool_arguments, stop, error };

constexpr std::string_view to_string(chat_event_kind kind) noexcept {
  switch (kind) {
  case chat_event_kind::text:
    return "text";
  case chat_event_kind::reasoning:
    return "reasoning";
  case chat_event_kind::tool_start:
    return "tool_start";
  case chat_event_kind::tool_arguments:
    return "tool_arguments";
  case chat_event_kind::stop:
    return "stop";
  case chat_event_kind::error:
    return "error";
  }
  return "unknown";
}

struct token_usage {
  int m_prompt = 0;
  int m_completion = 0;
  int m_total = 0;
  int m_cached = 0;
  int m_reasoning = 0;
};

struct chat_event {
  chat_event_kind m_kind = chat_event_kind::text;
  std::string m_text;
  int m_index = -1;
  std::string m_id;
  std::string m_name;
  std::string m_arguments;
  std::string m_reason;
  std::optional<token_usage> m_usage;
  int m_status = 0;
  std::string m_error;
  int m_retry_after = 0;
};

} // namespace qoderz::protocol
