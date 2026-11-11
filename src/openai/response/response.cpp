#include "openai/response/response.hpp"

#include "common/codec/random.hpp"
#include "qoder/errors.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::openai {
namespace {

using nlohmann::ordered_json;

constexpr std::string_view completion_prefix = "chatcmpl-";
constexpr std::string_view call_prefix = "call_";
constexpr std::string_view completion_object = "chat.completion";
constexpr std::string_view assistant_role = "assistant";
constexpr std::string_view default_finish_reason = "stop";
constexpr std::string_view tool_finish_reason = "tool_calls";
constexpr std::size_t generated_id_bytes = 16;

std::string make_call_id() {
  return std::string{call_prefix} + codec::random_hex(generated_id_bytes);
}

/// `Index >= 0` is the model's own numbering; a call without one continues the sequence, so two
/// unnamed starts never land on the same index.
int tool_index_of(protocol::chat_event const& event,
                  std::vector<detail::collected_tool_call>& tools) {
  return event.m_index >= 0 ? event.m_index : static_cast<int>(tools.size());
}

void append_tool_start(protocol::chat_event const& event, detail::completion_state& state) {
  detail::collected_tool_call call;
  call.m_index = tool_index_of(event, state.m_tools);
  call.m_id = event.m_id.empty() ? make_call_id() : event.m_id;
  call.m_name = event.m_name;
  call.m_arguments = event.m_arguments;
  state.m_tools.push_back(std::move(call));
}

void append_tool_arguments(protocol::chat_event const& event, detail::completion_state& state) {
  auto const found =
      std::find_if(state.m_tools.rbegin(), state.m_tools.rend(),
                   [&event](auto const& tool) { return tool.m_index == event.m_index; });
  if (found != state.m_tools.rend()) {
    found->m_arguments += event.m_arguments;
  }
}

void apply_event(protocol::chat_event const& event, detail::completion_state& state) {
  switch (event.m_kind) {
  case protocol::chat_event_kind::text:
    state.m_content += event.m_text;
    break;
  case protocol::chat_event_kind::reasoning:
    state.m_reasoning += event.m_text;
    break;
  case protocol::chat_event_kind::tool_start:
    append_tool_start(event, state);
    break;
  case protocol::chat_event_kind::tool_arguments:
    append_tool_arguments(event, state);
    break;
  case protocol::chat_event_kind::stop:
    state.m_stopped = true;
    state.m_reason = event.m_reason;
    state.m_usage = event.m_usage;
    break;
  case protocol::chat_event_kind::error:
    break;
  }
}

ordered_json write_tools(std::vector<detail::collected_tool_call> tools) {
  std::stable_sort(tools.begin(), tools.end(), [](auto const& left, auto const& right) {
    return left.m_index < right.m_index;
  });

  ordered_json written = ordered_json::array();
  for (auto const& tool : tools) {
    ordered_json function = ordered_json::object();
    function["name"] = tool.m_name;
    function["arguments"] = tool.m_arguments;
    written.push_back(ordered_json{
        {"index", tool.m_index},
        {"id", tool.m_id},
        {"type", "function"},
        {"function", std::move(function)},
    });
  }
  return written;
}

} // namespace

error detail::error_of(protocol::chat_event const& event) {
  auto classified =
      qoder::to_error(qoder::from_status(event.m_status, event.m_error, event.m_retry_after));
  if (!event.m_error.empty()) {
    classified.m_message = event.m_error;
  }
  return classified;
}

std::string make_completion_id() {
  return std::string{completion_prefix} + codec::random_hex(generated_id_bytes);
}

std::string detail::choose_finish_reason(completion_state const& state) {
  if (!state.m_reason.empty()) {
    return state.m_reason;
  }
  return state.m_tools.empty() ? std::string{default_finish_reason}
                               : std::string{tool_finish_reason};
}

ordered_json detail::make_usage(protocol::token_usage const& usage) {
  // The upstream total can be missing; the two halves are always known.
  auto const total = usage.m_total > 0 ? usage.m_total : usage.m_prompt + usage.m_completion;

  ordered_json written = ordered_json::object();
  written["prompt_tokens"] = usage.m_prompt;
  written["completion_tokens"] = usage.m_completion;
  written["total_tokens"] = total;
  written["prompt_tokens_details"] = ordered_json{{"cached_tokens", usage.m_cached}};
  written["completion_tokens_details"] = ordered_json{{"reasoning_tokens", usage.m_reasoning}};
  return written;
}

result<completion_result> build_completion(std::span<protocol::chat_event const> events,
                                           response_identity const& identity) {
  detail::completion_state state;
  for (auto const& event : events) {
    if (event.m_kind == protocol::chat_event_kind::error) {
      return std::unexpected(detail::error_of(event));
    }
    apply_event(event, state);
    if (state.m_stopped) {
      break;
    }
  }

  // The finish reason is decided before the tools are moved into the message.
  auto const finish_reason = detail::choose_finish_reason(state);

  ordered_json message = ordered_json::object();
  message["role"] = assistant_role;
  message["content"] = state.m_content;
  if (!state.m_reasoning.empty()) {
    message["reasoning_content"] = state.m_reasoning;
  }
  if (!state.m_tools.empty()) {
    message["tool_calls"] = write_tools(state.m_tools);
  }

  ordered_json choice = ordered_json::object();
  choice["index"] = 0;
  choice["message"] = std::move(message);
  choice["finish_reason"] = finish_reason;

  ordered_json document = ordered_json::object();
  document["id"] = identity.m_id;
  document["object"] = completion_object;
  document["created"] = identity.m_created;
  document["model"] = identity.m_model;
  document["choices"] = ordered_json::array({std::move(choice)});
  if (state.m_usage.has_value()) {
    document["usage"] = detail::make_usage(*state.m_usage);
  }

  completion_result built;
  built.m_json = document.dump();
  built.m_answered = state.m_stopped;
  return built;
}

} // namespace qoderz::openai
