#include "openai/request/normalize.hpp"

#include "common/codec/hash.hpp"
#include "common/strings.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::openai {
namespace {

constexpr std::size_t generated_id_hex = 32;
constexpr std::string_view generated_id_prefix = "call_";
constexpr std::string_view lost_call_id = "tool";
constexpr std::string_view image_turn_prefix = "[From the result of ";

std::string read_trimmed(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return {};
  }
  auto const value = object.find(name);
  if (value == object.end() || !value->is_string()) {
    return {};
  }
  return std::string{strings::trim(value->get_ref<std::string const&>())};
}

std::string read_part_text(json const& part) {
  if (part.is_string()) {
    return part.get<std::string>();
  }
  if (!part.is_object()) {
    return {};
  }
  auto const text = part.find("text");
  if (text == part.end() || !text->is_string()) {
    return {};
  }
  auto const type = read_trimmed(part, "type");
  if (type.empty() || type == "text" || type == "input_text") {
    return text->get<std::string>();
  }
  return {};
}

std::string read_image_url(json const& part) {
  auto const image = part.find("image_url");
  if (image == part.end()) {
    return {};
  }
  if (image->is_string()) {
    return std::string{strings::trim(image->get_ref<std::string const&>())};
  }
  if (!image->is_object()) {
    return {};
  }
  return read_trimmed(*image, "url");
}

message_block text_block(std::string text) {
  message_block block;
  block.m_kind = message_block::kind::text;
  block.m_text = std::move(text);
  return block;
}

message_block image_block(std::string url) {
  message_block block;
  block.m_kind = message_block::kind::image_url;
  block.m_url = std::move(url);
  return block;
}

json content_of(json const& message) {
  auto const content = message.find("content");
  return content == message.end() ? json{} : *content;
}

void append_user_blocks(json const& content, std::vector<message_block>& blocks) {
  if (content.is_string()) {
    blocks.push_back(text_block(content.get<std::string>()));
    return;
  }
  if (!content.is_array()) {
    return;
  }
  for (auto const& part : content) {
    if (part.is_string()) {
      blocks.push_back(text_block(part.get<std::string>()));
      continue;
    }
    if (!part.is_object()) {
      continue;
    }
    auto const type = read_trimmed(part, "type");
    if (type == "text") {
      auto const text = part.find("text");
      if (text != part.end() && text->is_string()) {
        blocks.push_back(text_block(text->get<std::string>()));
      }
    } else if (type == "image_url") {
      auto url = read_image_url(part);
      if (!url.empty()) {
        blocks.push_back(image_block(std::move(url)));
      }
    }
  }
}

void append_assistant_blocks(json const& content, std::vector<message_block>& blocks) {
  if (content.is_string()) {
    blocks.push_back(text_block(content.get<std::string>()));
    return;
  }
  if (!content.is_array()) {
    return;
  }
  for (auto const& part : content) {
    if (part.is_string()) {
      blocks.push_back(text_block(part.get<std::string>()));
      continue;
    }
    if (!part.is_object() || read_trimmed(part, "type") != "text") {
      continue;
    }
    auto const text = part.find("text");
    if (text != part.end() && text->is_string()) {
      blocks.push_back(text_block(text->get<std::string>()));
    }
  }
}

std::string read_arguments(json const& function) {
  auto const arguments = function.find("arguments");
  if (arguments == function.end() || arguments->is_null()) {
    return {};
  }
  if (arguments->is_string()) {
    return arguments->get<std::string>();
  }
  return arguments->dump();
}

std::string generated_id(int turn_index, int call_index, std::string_view name,
                         std::string_view arguments) {
  std::string seed;
  seed.reserve(name.size() + arguments.size() + 16);
  seed.append(std::to_string(turn_index));
  seed.push_back('\0');
  seed.append(std::to_string(call_index));
  seed.push_back('\0');
  seed.append(name);
  seed.push_back('\0');
  seed.append(arguments);

  auto hex = codec::sha256_hex(seed);
  hex.resize(generated_id_hex);
  return std::string{generated_id_prefix} + hex;
}

std::vector<tool_call> build_tool_calls(json const& message, int turn_index) {
  std::vector<tool_call> calls;
  auto const list = message.find("tool_calls");
  if (list == message.end() || !list->is_array()) {
    return calls;
  }

  int call_index = 0;
  for (auto const& entry : *list) {
    // A non-object keeps its slot in the numbering so a later generated id stays put.
    if (!entry.is_object()) {
      ++call_index;
      continue;
    }

    tool_call call;
    auto const function = entry.find("function");
    if (function != entry.end() && function->is_object()) {
      call.m_name = read_trimmed(*function, "name");
      call.m_arguments = read_arguments(*function);
    }
    call.m_id = read_trimmed(entry, "id");
    if (call.m_id.empty()) {
      call.m_id = generated_id(turn_index, call_index, call.m_name, call.m_arguments);
    }
    calls.push_back(std::move(call));
    ++call_index;
  }
  return calls;
}

std::string with_image_note(std::string text, std::size_t count) {
  auto const note =
      count == 1 ? std::string{"[The tool returned an image; it follows in the next message.]"}
                 : "[The tool returned " + std::to_string(count) +
                       " images; they follow in the next message.]";
  if (text.empty()) {
    return note;
  }
  return text + "\n\n" + note;
}

std::vector<message_block> read_images(json const& content) {
  std::vector<message_block> images;
  if (!content.is_array()) {
    return images;
  }
  for (auto const& part : content) {
    if (part.is_object() && read_trimmed(part, "type") == "image_url") {
      auto url = read_image_url(part);
      if (!url.empty()) {
        images.push_back(image_block(std::move(url)));
      }
    }
  }
  return images;
}

struct pending_call {
  std::string m_id;
  bool m_matched = false;
};

struct image_group {
  std::string m_call_id;
  std::vector<message_block> m_blocks;
};

// A tool result is a plain string upstream, so images it carries become their own user turn.
void flush_images(std::vector<normalized_message>& output, std::vector<image_group>& pending) {
  for (auto& group : pending) {
    normalized_message message;
    message.m_role = "user";
    message.m_blocks.push_back(text_block(std::string{image_turn_prefix} + group.m_call_id + ":]"));
    for (auto& block : group.m_blocks) {
      message.m_blocks.push_back(std::move(block));
    }
    message.m_array_content = true;
    output.push_back(std::move(message));
  }
  pending.clear();
}

int match_call(std::vector<pending_call> const& calls, std::string const& call_id) {
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (calls[index].m_matched) {
      continue;
    }
    // A named result never falls back to the earliest open call: answering the wrong tool is worse
    // than degrading the result to ordinary user text.
    if (call_id.empty() || calls[index].m_id == call_id) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void add_user(normalize_result& result, json const& message, int index, bool& has_user) {
  auto const content = content_of(message);

  normalized_message normalized;
  normalized.m_role = "user";
  append_user_blocks(content, normalized.m_blocks);
  normalized.m_array_content = content.is_array();

  auto const text = read_text_content(content);
  if (!has_user) {
    result.m_first_user_text = text;
    has_user = true;
  }
  result.m_last_user_index = index;
  result.m_last_user_text = text;
  result.m_messages.push_back(std::move(normalized));
}

void add_assistant(normalize_result& result, json const& message, int& assistant_turns,
                   std::vector<pending_call>& open) {
  auto calls = build_tool_calls(message, assistant_turns);
  if (!calls.empty()) {
    ++assistant_turns;
  }

  normalized_message normalized;
  normalized.m_role = "assistant";
  append_assistant_blocks(content_of(message), normalized.m_blocks);
  normalized.m_tool_calls = calls;
  result.m_messages.push_back(std::move(normalized));

  open.clear();
  for (auto const& call : calls) {
    open.push_back(pending_call{.m_id = call.m_id, .m_matched = false});
  }
}

void add_tool_result(normalize_result& result, json const& message, std::vector<pending_call>& open,
                     std::vector<image_group>& pending_images) {
  auto const call_id = read_trimmed(message, "tool_call_id");
  auto const content = content_of(message);

  auto text = read_text_content(content);
  auto images = read_images(content);
  auto const match = match_call(open, call_id);

  if (match < 0) {
    if (!text.empty()) {
      normalized_message orphan;
      orphan.m_role = "user";
      orphan.m_blocks.push_back(text_block(std::move(text)));
      orphan.m_array_content = content.is_array();
      result.m_messages.push_back(std::move(orphan));
    }
    if (!images.empty()) {
      image_group group;
      group.m_call_id = call_id.empty() ? std::string{lost_call_id} : call_id;
      group.m_blocks = std::move(images);
      pending_images.push_back(std::move(group));
    }
    return;
  }

  auto& call = open[static_cast<std::size_t>(match)];
  call.m_matched = true;

  if (!images.empty()) {
    text = with_image_note(std::move(text), images.size());
    image_group group;
    group.m_call_id = call.m_id;
    group.m_blocks = std::move(images);
    pending_images.push_back(std::move(group));
  }

  normalized_message tool;
  tool.m_role = "tool";
  tool.m_text = std::move(text);
  tool.m_tool_call_id = call.m_id;
  result.m_messages.push_back(std::move(tool));
}

void mark_cache(normalize_result& result) {
  if (result.m_messages.empty()) {
    return;
  }
  auto& last = result.m_messages.back();
  if (last.m_role != "user" || last.m_blocks.empty()) {
    return;
  }
  last.m_blocks.back().m_cache_control = true;
}

} // namespace

std::string read_text_content(json const& content) {
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (content.is_object()) {
    return read_part_text(content);
  }
  if (!content.is_array()) {
    return {};
  }

  std::string joined;
  for (auto const& part : content) {
    auto const text = read_part_text(part);
    if (text.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined.push_back('\n');
    }
    joined.append(text);
  }
  return joined;
}

result<normalize_result> normalize_messages(json const& messages) {
  if (!messages.is_array()) {
    return std::unexpected(bad_request_error("messages is required"));
  }

  normalize_result result;
  std::vector<image_group> pending_images;
  std::vector<pending_call> open_calls;
  int assistant_turns = 0;
  int index = 0;
  bool has_user = false;

  for (auto const& message : messages) {
    if (message.is_object()) {
      auto const role = read_trimmed(message, "role");
      if (role == "user") {
        flush_images(result.m_messages, pending_images);
        open_calls.clear();
        add_user(result, message, index, has_user);
      } else if (role == "assistant") {
        flush_images(result.m_messages, pending_images);
        add_assistant(result, message, assistant_turns, open_calls);
      } else if (role == "tool") {
        add_tool_result(result, message, open_calls, pending_images);
      }
    }
    ++index;
  }

  flush_images(result.m_messages, pending_images);
  mark_cache(result);
  return result;
}

} // namespace qoderz::openai
