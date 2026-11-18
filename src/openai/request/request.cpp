#include "openai/request/request.hpp"

#include "common/strings.hpp"
#include "openai/request/normalize.hpp"
#include "openai/selection/selectors.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::openai {
namespace {

constexpr int default_max_tokens = 32000;

constexpr std::string_view system_prompt =
    "You are a Qoder agent. Use the instructions below and the tools available to you to assist "
    "the user.";
constexpr std::string_view required_tools_line =
    "You must call an available function in this response.";
constexpr std::string_view default_parameters = R"({"type":"object","properties":{}})";

std::string read_trimmed_string(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return {};
  }
  auto const value = object.find(name);
  if (value == object.end() || !value->is_string()) {
    return {};
  }
  return std::string{strings::trim(value->get_ref<std::string const&>())};
}

std::optional<int> read_int32(json const& value) {
  if (value.is_number_unsigned()) {
    auto const number = value.get<std::uint64_t>();
    if (number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return static_cast<int>(number);
    }
    return std::nullopt;
  }
  if (value.is_number_integer()) {
    auto const number = value.get<std::int64_t>();
    if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max()) {
      return static_cast<int>(number);
    }
    return std::nullopt;
  }
  // `1234.0` is still a token count; a fraction is not one, and truncating it would
  // report a limit the client never asked for.
  if (value.is_number_float()) {
    auto const number = value.get<double>();
    if (number >= 1.0 && number <= static_cast<double>(std::numeric_limits<int>::max()) &&
        static_cast<double>(static_cast<std::int64_t>(number)) == number) {
      return static_cast<int>(number);
    }
  }
  return std::nullopt;
}

int read_max_tokens(json const& body) {
  if (!body.is_object()) {
    return default_max_tokens;
  }
  for (auto const name : {"max_completion_tokens", "max_tokens"}) {
    auto const value = body.find(name);
    if (value == body.end()) {
      continue;
    }
    if (auto const number = read_int32(*value); number.has_value() && *number > 0) {
      return *number;
    }
  }
  return default_max_tokens;
}

std::string read_tool_choice(json const& body) {
  if (!body.is_object()) {
    return {};
  }
  auto const choice = body.find("tool_choice");
  if (choice == body.end()) {
    return {};
  }
  if (choice->is_string()) {
    return strings::ascii_lower_copy(strings::trim(choice->get_ref<std::string const&>()));
  }
  // An object forces one specific function; nothing downstream can express that, so it is
  // reported as `function`, which the tool gate treats like `auto`.
  if (choice->is_object()) {
    return "function";
  }
  return {};
}

std::vector<tool_spec> read_tools(json const& body) {
  std::vector<tool_spec> tools;
  if (!body.is_object()) {
    return tools;
  }
  auto const list = body.find("tools");
  if (list == body.end() || !list->is_array()) {
    return tools;
  }

  for (auto const& entry : *list) {
    if (!entry.is_object()) {
      continue;
    }
    auto const function = entry.find("function");
    if (function == entry.end() || !function->is_object()) {
      continue;
    }
    auto name = read_trimmed_string(*function, "name");
    if (name.empty()) {
      continue;
    }

    tool_spec tool;
    tool.m_name = std::move(name);
    tool.m_description = read_trimmed_string(*function, "description");
    auto const parameters = function->find("parameters");
    if (parameters != function->end() && (parameters->is_object() || parameters->is_boolean())) {
      tool.m_parameters_json = parameters->dump();
    } else {
      tool.m_parameters_json = std::string{default_parameters};
    }
    tools.push_back(std::move(tool));
  }
  return tools;
}

std::string build_system_text(json const& messages, bool include_tools,
                              std::string_view tool_choice) {
  std::string text{system_prompt};
  std::string joined;

  for (auto const& message : messages) {
    if (!message.is_object()) {
      continue;
    }
    auto const role = read_trimmed_string(message, "role");
    if (role != "system" && role != "developer") {
      continue;
    }
    auto const content = message.find("content");
    auto const part = content == message.end() ? std::string{} : read_text_content(*content);
    if (part.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined.append("\n\n");
    }
    joined.append(part);
  }

  if (!joined.empty()) {
    text.append("\n\n");
    text.append(joined);
  }
  if (include_tools && tool_choice == "required") {
    text.append("\n");
    text.append(required_tools_line);
  }
  return text;
}

template <class prepared_type>
result<request_context> prepare_request_impl(prepared_type&& prepared,
                                             qoder::catalog::model_info const& model,
                                             selection::session_store& sessions, clock& time) {
  request_context context;
  context.m_model = model;
  context.m_system_text = std::forward<prepared_type>(prepared).m_system_text;
  context.m_session_id = prepared.m_session_id;

  auto const effort = selection::select_effort(model, prepared.m_requested_effort);
  context.m_enable_thinking = effort.m_enable_thinking;
  context.m_reasoning_effort = effort.m_effort;
  context.m_max_tokens = prepared.m_max_tokens;
  context.m_tools = std::forward<prepared_type>(prepared).m_tools;
  context.m_include_tools = prepared.m_include_tools;
  context.m_messages = std::forward<prepared_type>(prepared).m_dialog.m_messages;
  context.m_task = sessions.get_or_create(context.m_session_id, prepared.m_dialog.m_last_user_index,
                                          prepared.m_dialog.m_last_user_text, time.now_ms());
  context.m_request_id = selection::make_uuid();
  return context;
}

} // namespace

result<prepared_request> read_request(json const& body,
                                      std::optional<std::string_view> session_header,
                                      selection::session_store const& sessions) {
  auto const messages = body.find("messages");
  if (messages == body.end() || !messages->is_array()) {
    return std::unexpected(bad_request_error("messages is required"));
  }

  auto const tool_choice = read_tool_choice(body);

  prepared_request prepared;
  prepared.m_tools = read_tools(body);
  prepared.m_include_tools = !prepared.m_tools.empty() && tool_choice != "none";
  prepared.m_system_text = build_system_text(*messages, prepared.m_include_tools, tool_choice);

  auto dialog = normalize_messages(*messages);
  if (!dialog.has_value()) {
    return std::unexpected(std::move(dialog.error()));
  }
  prepared.m_dialog = std::move(*dialog);

  auto const cache_key = read_trimmed_string(body, "prompt_cache_key");
  auto const user = read_trimmed_string(body, "user");
  prepared.m_session_id = sessions.make_session_id(
      session_header, cache_key, user, prepared.m_system_text, prepared.m_dialog.m_first_user_text);
  std::string{}.swap(prepared.m_dialog.m_first_user_text);
  prepared.m_requested_effort = read_trimmed_string(body, "reasoning_effort");
  prepared.m_max_tokens = read_max_tokens(body);
  return prepared;
}

result<request_context> prepare_request(prepared_request const& prepared,
                                        qoder::catalog::model_info const& model,
                                        selection::session_store& sessions, clock& time) {
  return prepare_request_impl(prepared, model, sessions, time);
}

result<request_context> prepare_request(prepared_request&& prepared,
                                        qoder::catalog::model_info const& model,
                                        selection::session_store& sessions, clock& time) {
  return prepare_request_impl(std::move(prepared), model, sessions, time);
}

result<request_context> prepare_request(json const& body,
                                        std::optional<std::string_view> session_header,
                                        qoder::catalog::model_info const& model,
                                        selection::session_store& sessions, clock& time) {
  auto prepared = read_request(body, session_header, sessions);
  if (!prepared) {
    return std::unexpected(std::move(prepared.error()));
  }
  return prepare_request(std::move(*prepared), model, sessions, time);
}

} // namespace qoderz::openai
