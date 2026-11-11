#include "openai/response/stream_frames.hpp"

#include "qoder/errors.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::openai {
namespace {

using nlohmann::ordered_json;

constexpr std::string_view stream_object = "chat.completion.chunk";
constexpr std::string_view assistant_role = "assistant";
constexpr std::string_view data_prefix = "data: ";
constexpr std::string_view frame_terminator = "\n\n";
constexpr std::string_view done_frame = "data: [DONE]\n\n";
constexpr std::string_view stop_reason = "stop";
constexpr std::string_view error_type = "qoder_error";

int frame_index(protocol::chat_event const& event) noexcept {
  return event.m_index > 0 ? event.m_index : 0;
}

ordered_json tool_calls_of(ordered_json call) {
  return ordered_json::array({std::move(call)});
}

ordered_json delta_of(protocol::chat_event const& event) {
  switch (event.m_kind) {
  case protocol::chat_event_kind::text:
    return ordered_json{{"content", event.m_text}};
  case protocol::chat_event_kind::reasoning:
    return ordered_json{{"reasoning_content", event.m_text}};
  case protocol::chat_event_kind::tool_start:
    return ordered_json{
        {"tool_calls",
         tool_calls_of(ordered_json{
             {"index", frame_index(event)},
             {"id", event.m_id},
             {"type", "function"},
             {"function", ordered_json{{"name", event.m_name}, {"arguments", event.m_arguments}}},
         })}};
  case protocol::chat_event_kind::tool_arguments:
    return ordered_json{
        {"tool_calls", tool_calls_of(ordered_json{
                           {"index", frame_index(event)},
                           {"function", ordered_json{{"arguments", event.m_arguments}}},
                       })}};
  case protocol::chat_event_kind::stop:
  case protocol::chat_event_kind::error:
    break;
  }
  return ordered_json::object();
}

} // namespace

stream_translator::stream_translator(response_identity identity)
    : m_identity{std::move(identity)} {}

std::string stream_translator::frame(ordered_json const& chunk) const {
  return std::string{data_prefix} + chunk.dump() + std::string{frame_terminator};
}

ordered_json stream_translator::chunk_with(ordered_json delta, ordered_json finish_reason) const {
  ordered_json choice = ordered_json::object();
  choice["index"] = 0;
  choice["delta"] = std::move(delta);
  choice["finish_reason"] = std::move(finish_reason);

  ordered_json chunk = ordered_json::object();
  chunk["id"] = m_identity.m_id;
  chunk["object"] = stream_object;
  chunk["created"] = m_identity.m_created;
  chunk["model"] = m_identity.m_model;
  chunk["choices"] = ordered_json::array({std::move(choice)});
  return chunk;
}

void stream_translator::append_role_frame(std::vector<std::string>& frames) {
  if (m_role_sent) {
    return;
  }
  m_role_sent = true;
  frames.push_back(
      frame(chunk_with(ordered_json{{"role", assistant_role}, {"content", ""}}, nullptr)));
}

std::string stream_translator::error_frame(protocol::chat_event const& event) const {
  auto const error = detail::error_of(event);
  // No fallback is needed: `qoder::from_status` always classifies into a positive status and a
  // non-empty message (an unknown status becomes 502, a blank message becomes the status sentence),
  // so the branches that guarded those two fields could never run.
  auto const status = error.m_http_status;

  ordered_json body = ordered_json::object();
  body["message"] = error.m_message;
  body["type"] = error_type;
  body["code"] = status;

  return std::string{data_prefix} + ordered_json{{"error", std::move(body)}}.dump() +
         std::string{frame_terminator};
}

std::vector<std::string> stream_translator::on_event(protocol::chat_event const& event) {
  std::vector<std::string> frames;
  if (m_terminal) {
    return frames;
  }

  if (event.m_kind == protocol::chat_event_kind::error) {
    m_terminal = true;
    m_failed = true;
    frames.push_back(error_frame(event));
    return frames;
  }

  append_role_frame(frames);

  if (event.m_kind != protocol::chat_event_kind::stop) {
    frames.push_back(frame(chunk_with(delta_of(event), nullptr)));
    return frames;
  }

  auto const reason = event.m_reason.empty() ? std::string{stop_reason} : event.m_reason;
  ordered_json stop_chunk = chunk_with(ordered_json::object(), ordered_json(std::move(reason)));
  if (event.m_usage.has_value()) {
    stop_chunk["usage"] = detail::make_usage(*event.m_usage);
  }
  frames.push_back(frame(stop_chunk));
  frames.emplace_back(done_frame);

  m_answered = true;
  m_terminal = true;
  return frames;
}

std::string stream_translator::finish() {
  if (m_terminal) {
    return {};
  }
  m_terminal = true;
  return std::string{done_frame};
}

bool stream_translator::answered() const noexcept {
  return m_answered;
}

bool stream_translator::failed() const noexcept {
  return m_failed;
}

} // namespace qoderz::openai
