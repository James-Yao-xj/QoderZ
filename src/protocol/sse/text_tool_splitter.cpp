#include "protocol/sse/text_tool_splitter.hpp"

#include "common/codec/random.hpp"
#include "common/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::protocol {
namespace {

constexpr std::string_view call_open = "<tool_call>";
constexpr std::string_view call_close = "</tool_call>";
constexpr std::string_view function_open = "<function=";
constexpr std::string_view function_close = "</function>";
constexpr std::string_view parameter_open = "<parameter=";
constexpr std::string_view parameter_close = "</parameter>";
constexpr std::size_t fence_length = 3;
constexpr int error_status = 502;

constexpr std::string_view unreadable_message =
    "the model's tool call, written as text, can't be read; it is not sent on as the answer";
constexpr std::string_view cut_short_message =
    "the model's tool call, written as text, was cut short; it is not sent on as the answer";

using ordered_json = nlohmann::ordered_json;

constexpr bool is_odd(std::size_t value) noexcept {
  return (value & 1U) != 0;
}

/// nlohmann parses `NaN`/`Infinity`, the reference parser rejects them.
bool keep_number(int /*depth*/, ordered_json::parse_event_t event, ordered_json& parsed) {
  if (event == ordered_json::parse_event_t::value && parsed.is_number_float()) {
    auto const value = parsed.get<double>();
    return std::isfinite(value);
  }
  return true;
}

/// The reference parser caps nesting at 64 levels (JsonDocument.MaxDepth); deeper input is
/// rejected before nlohmann sees it, because recursion would overflow the stack.
constexpr std::size_t max_json_depth = 64;

bool exceeds_json_depth(std::string_view text) noexcept {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char const character : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }

    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      if (++depth > max_json_depth) {
        return true;
      }
    } else if ((character == '}' || character == ']') && depth > 0) {
      --depth;
    }
  }
  return false;
}

// Parentheses matter: brace init would build a one-element array around the discarded value.
ordered_json discarded_json() {
  return ordered_json(ordered_json::value_t::discarded);
}

ordered_json parse_strict(std::string_view text) {
  if (exceeds_json_depth(text)) {
    return discarded_json();
  }
  try {
    return ordered_json::parse(text, keep_number, false);
  } catch (nlohmann::json::exception const&) {
    return discarded_json();
  }
}

bool is_callish(std::string_view body) noexcept {
  return strings::starts_with(strings::trim_start(body), "{") ||
         body.find(function_open) != std::string_view::npos;
}

std::optional<tool_call_text> parse_json_call(std::string_view body) {
  auto const text = strings::trim(body);
  if (!strings::starts_with(text, "{")) {
    return std::nullopt;
  }

  auto const document = parse_strict(text);
  if (document.is_discarded() || !document.is_object()) {
    return std::nullopt;
  }

  auto const name_entry = document.find("name");
  if (name_entry == document.end() || !name_entry->is_string()) {
    return std::nullopt;
  }

  auto name = std::string{strings::trim(name_entry->get_ref<std::string const&>())};
  if (name.empty()) {
    return std::nullopt;
  }

  auto const arguments_entry = document.find("arguments");
  if (arguments_entry == document.end() || !arguments_entry->is_object()) {
    return std::nullopt;
  }

  return tool_call_text{.m_name = std::move(name),
                        .m_arguments = arguments_entry->dump(
                            -1, ' ', false, nlohmann::json::error_handler_t::replace)};
}

void write_parameter(ordered_json& arguments, std::string_view key, std::string_view value) {
  auto parsed = parse_strict(value);
  if (parsed.is_discarded()) {
    arguments[std::string{key}] = std::string{value};
    return;
  }
  arguments[std::string{key}] = std::move(parsed);
}

std::optional<tool_call_text> parse_xml_call(std::string_view body) {
  auto const open = body.find(function_open);
  if (open == std::string_view::npos) {
    return std::nullopt;
  }

  auto const name_start = open + function_open.size();
  auto const name_end_offset = body.substr(name_start).find('>');
  if (name_end_offset == std::string_view::npos) {
    return std::nullopt;
  }
  auto const name_end = name_start + name_end_offset;

  auto name = std::string{strings::trim(body.substr(name_start, name_end - name_start))};
  if (name.empty()) {
    return std::nullopt;
  }

  auto const inner_start = name_end + 1;
  auto const close_offset = body.substr(inner_start).find(function_close);
  if (close_offset == std::string_view::npos) {
    return std::nullopt;
  }
  auto const inner = body.substr(inner_start, close_offset);

  ordered_json arguments = ordered_json::object();
  std::size_t offset = 0;
  while (offset < inner.size()) {
    auto const parameter_offset = inner.substr(offset).find(parameter_open);
    if (parameter_offset == std::string_view::npos) {
      break;
    }

    auto const key_start = offset + parameter_offset + parameter_open.size();
    auto const key_end_offset = inner.substr(key_start).find('>');
    if (key_end_offset == std::string_view::npos) {
      break;
    }
    auto const key_end = key_start + key_end_offset;

    auto const value_start = key_end + 1;
    auto const value_end_offset = inner.substr(value_start).find(parameter_close);
    if (value_end_offset == std::string_view::npos) {
      break;
    }
    auto const value_end = value_start + value_end_offset;

    auto const key = strings::trim(inner.substr(key_start, key_end - key_start));
    if (!key.empty()) {
      write_parameter(arguments, key,
                      strings::trim(inner.substr(value_start, value_end - value_start)));
    }

    offset = value_end + parameter_close.size();
  }

  return tool_call_text{
      .m_name = std::move(name),
      .m_arguments = arguments.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
}

std::size_t longest_open_prefix(std::string_view combined) noexcept {
  auto const limit = std::min(call_open.size() - 1, combined.size());
  for (std::size_t length = limit; length > 0; --length) {
    if (combined.substr(combined.size() - length) == call_open.substr(0, length)) {
      return length;
    }
  }
  return 0;
}

} // namespace

std::optional<tool_call_text> text_tool_splitter::parse_call(std::string_view body) {
  if (auto call = parse_json_call(body)) {
    return call;
  }
  return parse_xml_call(body);
}

bool text_tool_splitter::saw_tool() const noexcept {
  return m_saw_tool;
}

bool text_tool_splitter::is_quoted() const noexcept {
  return (m_fence_open != is_odd(m_run_length / fence_length)) ||
         (m_line_backtick_odd != is_odd(m_run_length % fence_length));
}

void text_tool_splitter::track_fence(std::string_view text) noexcept {
  for (char const character : text) {
    if (character == '`') {
      ++m_run_length;
      continue;
    }

    m_fence_open = m_fence_open != is_odd(m_run_length / fence_length);
    m_line_backtick_odd = m_line_backtick_odd != is_odd(m_run_length % fence_length);
    m_run_length = 0;
    if (character == '\n') {
      m_line_backtick_odd = false;
    }
  }
}

void text_tool_splitter::emit_text(std::vector<chat_event>& events, std::string_view text) {
  if (text.empty()) {
    return;
  }
  track_fence(text);

  chat_event event;
  event.m_kind = chat_event_kind::text;
  event.m_text = std::string{text};
  events.push_back(std::move(event));
}

chat_event text_tool_splitter::make_tool_event(tool_call_text const& call) {
  chat_event event;
  event.m_kind = chat_event_kind::tool_start;
  event.m_index = m_next_index++;
  event.m_id = "call_" + codec::random_hex(16);
  event.m_name = call.m_name;
  event.m_arguments = call.m_arguments;
  return event;
}

std::vector<chat_event> text_tool_splitter::feed(std::string_view chunk) {
  std::vector<chat_event> events;
  if (chunk.empty()) {
    return events;
  }

  // While a call is open the body accumulates in `m_call_buffer` and `m_call_scanned` records how
  // far the search for the closing tag already got, so an unclosed body is neither copied through a
  // scratch string nor scanned again on every feed. `pending` carries only the partial open tag
  // (never longer than `call_open.size() - 1`) plus this chunk.
  std::string pending;
  if (m_in_call) {
    m_call_buffer.append(chunk);
  } else {
    pending.reserve(m_text_buffer.size() + chunk.size());
    pending.append(m_text_buffer);
    pending.append(chunk);
    m_text_buffer.clear();
  }

  std::size_t position = 0;
  while (true) {
    if (!m_in_call) {
      if (position >= pending.size()) {
        return events;
      }

      auto const open = pending.find(call_open, position);
      if (open == std::string::npos) {
        auto const rest = std::string_view{pending}.substr(position);
        auto const keep = longest_open_prefix(rest);
        if (keep < rest.size()) {
          emit_text(events, rest.substr(0, rest.size() - keep));
        }
        m_text_buffer.assign(rest.substr(rest.size() - keep));
        return events;
      }

      emit_text(events, std::string_view{pending}.substr(position, open - position));
      position = open + call_open.size();
      m_in_call = true;
      m_call_buffer.assign(pending, position, std::string::npos);
      m_call_scanned = 0;
    }

    auto const close = m_call_buffer.find(call_close, m_call_scanned);
    if (close == std::string::npos) {
      // Re-scan only the tail that could still begin a closing tag.
      auto const overlap = call_close.size() - 1;
      m_call_scanned = m_call_buffer.size() > overlap ? m_call_buffer.size() - overlap : 0;
      return events;
    }

    auto const body = std::string_view{m_call_buffer}.substr(0, close);
    if (auto call = parse_call(body)) {
      m_saw_tool = true;
      events.push_back(make_tool_event(*call));
    } else if (is_callish(body) && !is_quoted()) {
      m_in_call = false;
      m_call_buffer.clear();
      m_call_scanned = 0;

      chat_event event;
      event.m_kind = chat_event_kind::error;
      event.m_status = error_status;
      event.m_error = std::string{unreadable_message};
      events.push_back(std::move(event));
      return events;
    } else {
      std::string re_emitted;
      re_emitted.reserve(call_open.size() + body.size() + call_close.size());
      re_emitted.append(call_open);
      re_emitted.append(body);
      re_emitted.append(call_close);
      emit_text(events, re_emitted);
    }

    // The bytes after the closing tag are ordinary text again; scan them from their start.
    pending.assign(m_call_buffer, close + call_close.size(), std::string::npos);
    position = 0;
    m_in_call = false;
    m_call_buffer.clear();
    m_call_scanned = 0;
  }
}

std::vector<chat_event> text_tool_splitter::flush() {

  std::vector<chat_event> events;

  if (m_in_call) {
    std::string body;
    body.swap(m_call_buffer);
    m_call_scanned = 0;
    m_in_call = false;

    if (is_callish(body) && !is_quoted()) {
      auto call = parse_call(body);
      if (!call) {
        chat_event event;
        event.m_kind = chat_event_kind::error;
        event.m_status = error_status;
        event.m_error = std::string{cut_short_message};
        events.push_back(std::move(event));
        return events;
      }

      m_saw_tool = true;
      events.push_back(make_tool_event(*call));
      return events;
    }

    std::string text;
    text.reserve(call_open.size() + body.size());
    text.append(call_open);
    text.append(body);
    emit_text(events, text);
    return events;
  }

  if (m_text_buffer.empty()) {
    return events;
  }

  std::string text;
  text.swap(m_text_buffer);
  emit_text(events, text);
  return events;
}

} // namespace qoderz::protocol
