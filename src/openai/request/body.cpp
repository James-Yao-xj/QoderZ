#include "openai/request/body.hpp"

#include "common/json.hpp"
#include "openai/selection/selectors.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace qoderz::openai {
namespace {

constexpr std::string_view qoder_product = "app";
constexpr std::string_view qoder_version = "1.1.69";
constexpr std::string_view qoder_type = "agent";
constexpr std::string_view agent_id = "agent_common";
constexpr std::string_view task_id = "common";
constexpr std::string_view session_type = "app";
constexpr std::size_t empty_tools_bytes = 2;

using ordered_json = nlohmann::ordered_json;

// A listing that cannot be read back is better as `{}` than as a failed request.
ordered_json parse_model_config(std::string const& raw) {
  auto parsed = ordered_json::parse(raw, nullptr, false);
  if (raw.empty() || parsed.is_discarded() || !parsed.is_object()) {
    return ordered_json::object();
  }
  return parsed;
}

// A schema is kept as the client wrote it, `true`/`false` included: only unreadable text falls
// back to the empty object schema.
ordered_json parse_parameters(std::string const& raw) {
  auto parsed = ordered_json::parse(raw, nullptr, false);
  if (parsed.is_discarded()) {
    return ordered_json::object();
  }
  return parsed;
}

ordered_json write_block(message_block const& block) {
  ordered_json written = ordered_json::object();
  if (block.m_kind == message_block::kind::image_url) {
    written["type"] = "image_url";
    written["image_url"] = ordered_json{{"url", block.m_url}};
  } else {
    written["type"] = "text";
    written["text"] = block.m_text;
  }
  if (block.m_cache_control) {
    written["cache_control"] = ordered_json{{"type", "ephemeral"}};
  }
  return written;
}

ordered_json write_blocks(std::vector<message_block> const& blocks) {
  ordered_json written = ordered_json::array();
  for (auto const& block : blocks) {
    written.push_back(write_block(block));
  }
  return written;
}

ordered_json write_system_message(std::string const& system_text) {
  return ordered_json{
      {"role", "system"},
      {"content", ordered_json::array({ordered_json{{"type", "text"}, {"text", system_text}}})}};
}

ordered_json write_message(normalized_message const& message) {
  if (message.m_role == "tool") {
    return ordered_json{
        {"role", "tool"}, {"tool_call_id", message.m_tool_call_id}, {"content", message.m_text}};
  }

  ordered_json written = ordered_json::object();
  written["role"] = message.m_role == "assistant" ? "assistant" : "user";
  written["content"] = write_blocks(message.m_blocks);
  if (message.m_role == "assistant") {
    if (!message.m_tool_calls.empty()) {
      ordered_json calls = ordered_json::array();
      for (auto const& call : message.m_tool_calls) {
        calls.push_back(ordered_json{
            {"id", call.m_id},
            {"type", "function"},
            {"function", ordered_json{{"name", call.m_name}, {"arguments", call.m_arguments}}},
        });
      }
      written["tool_calls"] = std::move(calls);
    }
  } else if (message.m_array_content) {
    written["contents"] = write_blocks(message.m_blocks);
  }
  return written;
}

ordered_json write_messages(request_context const& context) {
  ordered_json written = ordered_json::array();
  written.push_back(write_system_message(context.m_system_text));
  for (auto const& message : context.m_messages) {
    written.push_back(write_message(message));
  }
  return written;
}

ordered_json write_tools(std::vector<tool_spec> const& tools) {
  ordered_json written = ordered_json::array();
  for (auto const& tool : tools) {
    ordered_json function = ordered_json::object();
    function["name"] = tool.m_name;
    function["description"] = tool.m_description;
    function["parameters"] = parse_parameters(tool.m_parameters_json);
    written.push_back(ordered_json{{"type", "function"}, {"function", std::move(function)}});
  }
  return written;
}

} // namespace

result<built_body> build_qoder_body(request_context const& context) {
  built_body built;

  auto const messages = write_messages(context);
  auto const messages_bytes = messages.dump().size();
  auto const tools = context.m_include_tools ? write_tools(context.m_tools) : ordered_json::array();
  auto const tools_bytes = context.m_include_tools ? tools.dump().size() : empty_tools_bytes;

  auto const context_length =
      selection::select_window(context.m_model, messages_bytes, tools_bytes, context.m_max_tokens);

  ordered_json parameters = ordered_json::object();
  parameters["enable_thinking"] = context.m_enable_thinking;
  parameters["max_tokens"] = context.m_max_tokens;
  if (!context.m_reasoning_effort.empty()) {
    parameters["reasoning_effort"] = context.m_reasoning_effort;
  }
  if (context_length > 0) {
    parameters["context_length"] = context_length;
  }

  ordered_json business = ordered_json::object();
  business["product"] = qoder_product;
  business["version"] = qoder_version;
  business["type"] = qoder_type;
  business["id"] = context.m_task.m_id;
  business["name"] = context.m_task.m_name;
  business["begin_at"] = context.m_task.m_begin_at_ms;
  business["stage"] = context.m_task.m_answered ? "processing" : "start";

  ordered_json body = ordered_json::object();
  body["request_id"] = context.m_request_id;
  body["request_set_id"] = context.m_task.m_id;
  body["chat_record_id"] = context.m_request_id;
  body["session_id"] = context.m_session_id;
  body["business"] = std::move(business);
  body["agent_id"] = agent_id;
  body["task_id"] = task_id;
  body["session_type"] = session_type;
  body["model_config"] = parse_model_config(context.m_model.m_raw_json);
  body["system"] =
      ordered_json::array({ordered_json{{"type", "text"}, {"text", context.m_system_text}}});
  body["messages"] = messages;
  if (context.m_include_tools) {
    body["tools"] = tools;
  }
  body["parameters"] = std::move(parameters);

  built.m_json = body.dump();
  built.m_context_length = context_length > 0 ? context_length : 0;
  return built;
}

} // namespace qoderz::openai
