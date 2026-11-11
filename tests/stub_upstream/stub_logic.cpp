#include "stub_upstream/stub_logic.hpp"

#include "common/json.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::test {
namespace {

using qoderz::json;

constexpr std::string_view listing_suffix = "/algo/api/v2/model/list";
constexpr std::string_view chat_suffix = "agent_chat_generation";
constexpr std::string_view usage_suffix = "/me/usage";
constexpr std::string_view job_refresh_suffix = "Token/refresh";
constexpr std::string_view job_refresh_suffix_lower = "token/refresh";
constexpr std::string_view json_type = "application/json";
constexpr std::string_view event_stream_type = "text/event-stream";

bool contains(std::string_view text, std::string_view needle) noexcept {
  return text.find(needle) != std::string_view::npos;
}

void set_header(stub_response& response, std::string name, std::string value) {
  response.m_headers.push_back(
      qoderz::net::http_header{.m_name = std::move(name), .m_value = std::move(value)});
}

/// One `data:` line of the Qoder SSE envelope: the inner chunk travels as a JSON *string* inside
/// `body`, which is what the protocol fixture of `docs/protocol/qoder-chat.md` §9 shows.
std::string sse_frame(std::string const& inner) {
  json envelope{{"statusCodeValue", 200}, {"body", inner}};
  std::string frame = "data: ";
  frame.append(envelope.dump());
  frame.append("\n\n");
  return frame;
}

std::vector<std::string> chat_frames(stub_upstream_options const& options) {
  std::vector<std::string> frames;
  frames.reserve(options.m_chunks + 2);
  std::string const content(options.m_chunk_bytes, 'd');

  for (std::size_t index = 0; index < options.m_chunks; ++index) {
    json choice{{"index", 0}, {"delta", json{{"content", content}}}};
    if (index + 1 == options.m_chunks) {
      choice["finish_reason"] = "stop";
    }
    json inner{{"choices", json::array({std::move(choice)})}};
    if (index + 1 == options.m_chunks) {
      // The last frame carries the usage the gateway copies into the OpenAI `usage` object.
      auto const completion = static_cast<long long>(options.m_chunks);
      inner["usage"] = json{{"prompt_tokens", 1000},
                            {"completion_tokens", completion},
                            {"total_tokens", 1000 + completion}};
    }
    frames.push_back(sse_frame(inner.dump()));
  }

  frames.push_back(sse_frame("[DONE]"));
  return frames;
}

stub_response json_answer(std::string body) {
  stub_response response;
  response.m_status = 200;
  set_header(response, "Content-Type", std::string{json_type});
  response.m_body = std::move(body);
  return response;
}

} // namespace

std::string_view stub_path_of(std::string_view target) noexcept {
  auto const query = target.find('?');
  return query == std::string_view::npos ? target : target.substr(0, query);
}

std::string stub_model_listing_body() {
  json const model{
      {"key", stub_model_key},
      {"display_name", stub_model_name},
      {"enable", true},
      {"max_input_tokens", 1000000},
      {"is_vl", false},
      {"context_config", json{{"long", json{{"token_count", 1000000}, {"is_default", true}}},
                              {"short", json{{"token_count", 200000}, {"is_default", false}}}}},
      {"thinking_config",
       json{{"enabled", json{{"efforts", json{{"low", json::object()},
                                              {"medium", json{{"is_default", true}}},
                                              {"high", json::object()}}}}},
            {"disabled", json::object()}}},
  };
  return json{{"app", json::array({model})}, {"chat", json::array()}}.dump();
}

std::size_t stub_completion_length(stub_upstream_options const& options) noexcept {
  return options.m_chunks * options.m_chunk_bytes;
}

std::string stub_chat_stream_body(stub_upstream_options const& options) {
  std::string body;
  for (auto const& frame : chat_frames(options)) {
    body.append(frame);
  }
  return body;
}

std::string stub_job_token_body() {
  return json{{"token", "stub-access-token-not-real"},
              {"refresh_token", "stub-refresh-token-not-real"},
              {"expires_at", 4102444800000LL}}
      .dump();
}

std::string stub_usage_body() {
  // A legal `/me/usage` answer: `displayMode` must be `qoder` and at least one window has to carry
  // a usable total, otherwise `qoderz usage` reports "missing quota data" and exits 4.
  json const window{{"total", 1000}, {"used", 250}, {"unit", "credits"}};
  return json{{"displayMode", "qoder"},
              {"userType", "pro_trial"},
              {"expiresAt", 1767225600000LL},
              {"qoderUsage", json{{"userQuota", window}}}}
      .dump();
}

stub_route_result stub_route(stub_request const& request, stub_upstream_options const& options) {
  auto const path = stub_path_of(request.m_target);

  if (contains(path, listing_suffix)) {
    return stub_route_result{.m_response = json_answer(stub_model_listing_body())};
  }

  if (contains(path, chat_suffix)) {
    stub_response response;
    response.m_status = 200;
    set_header(response, "Content-Type", std::string{event_stream_type});
    set_header(response, "Cache-Control", "no-cache");
    set_header(response, "X-Accel-Buffering", "no");
    response.m_chunked = true;
    response.m_chunks = chat_frames(options);
    return stub_route_result{.m_response = std::move(response)};
  }

  if (contains(path, job_refresh_suffix) || contains(path, job_refresh_suffix_lower)) {
    return stub_route_result{.m_response = json_answer(stub_job_token_body())};
  }

  if (contains(path, usage_suffix)) {
    return stub_route_result{.m_response = json_answer(stub_usage_body())};
  }

  return stub_route_result{.m_response = json_answer("{}"), .m_handled = false};
}

} // namespace qoderz::test
