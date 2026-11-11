#pragma once

#include "protocol/sse/chat_event.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::protocol {

struct tool_call_text {
  std::string m_name;
  std::string m_arguments;
};

/// Splits assistant text that encodes tool calls as `<tool_call>...` blocks into events. Calls that
/// cannot be read raise a 502 error event; calls inside a fence or an odd inline backtick run are
/// re-emitted as text instead. Not thread-safe.
class text_tool_splitter {
public:
  std::vector<chat_event> feed(std::string_view chunk);
  std::vector<chat_event> flush();

  [[nodiscard]] bool saw_tool() const noexcept;
  [[nodiscard]] bool is_quoted() const noexcept;

  /// JSON first (`{"name":...,"arguments":{...}}`), then XML
  /// (`<function=name><parameter=key>value</parameter></function>`). Empty when neither matches.
  [[nodiscard]] static std::optional<tool_call_text> parse_call(std::string_view body);

private:
  chat_event make_tool_event(tool_call_text const& call);
  void emit_text(std::vector<chat_event>& events, std::string_view text);
  void track_fence(std::string_view text) noexcept;

  std::string m_text_buffer;
  std::string m_call_buffer;
  /// Offset in `m_call_buffer` from which the closing tag is still unscanned; keeps an open call
  /// linear across feeds by never rescanning bytes that already proved not to start one.
  std::size_t m_call_scanned = 0;
  bool m_in_call = false;
  bool m_fence_open = false;
  bool m_line_backtick_odd = false;
  std::size_t m_run_length = 0;
  bool m_saw_tool = false;
  int m_next_index = 0;
};

} // namespace qoderz::protocol
