#include "common/error.hpp"
#include "openai/response/response.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::openai::build_completion;
using qoderz::openai::completion_result;
using qoderz::openai::response_identity;
using qoderz::protocol::chat_event;
using qoderz::protocol::chat_event_kind;
using qoderz::protocol::token_usage;

constexpr std::string_view fixed_id = "chatcmpl-0123456789abcdef0123456789abcdef";

response_identity fixture_identity() {
  return {.m_id = std::string{fixed_id}, .m_created = 1760000000, .m_model = "dmodel"};
}

chat_event text_event(std::string text) {
  chat_event event;
  event.m_kind = chat_event_kind::text;
  event.m_text = std::move(text);
  return event;
}

chat_event reasoning_event(std::string text) {
  chat_event event;
  event.m_kind = chat_event_kind::reasoning;
  event.m_text = std::move(text);
  return event;
}

chat_event tool_start(int index, std::string id, std::string name, std::string arguments) {
  chat_event event;
  event.m_kind = chat_event_kind::tool_start;
  event.m_index = index;
  event.m_id = std::move(id);
  event.m_name = std::move(name);
  event.m_arguments = std::move(arguments);
  return event;
}

chat_event tool_arguments(int index, std::string arguments) {
  chat_event event;
  event.m_kind = chat_event_kind::tool_arguments;
  event.m_index = index;
  event.m_arguments = std::move(arguments);
  return event;
}

chat_event stop_event(std::string reason, std::optional<token_usage> usage = std::nullopt) {
  chat_event event;
  event.m_kind = chat_event_kind::stop;
  event.m_reason = std::move(reason);
  event.m_usage = usage;
  return event;
}

chat_event error_event(int status, std::string message) {
  chat_event event;
  event.m_kind = chat_event_kind::error;
  event.m_status = status;
  event.m_error = std::move(message);
  return event;
}

qoderz::result<completion_result> build(std::vector<chat_event> const& events) {
  return build_completion(std::span<chat_event const>{events}, fixture_identity());
}

constexpr token_usage sample_usage() {
  return {.m_prompt = 10, .m_completion = 5, .m_total = 15, .m_cached = 2, .m_reasoning = 3};
}

} // namespace

TEST_CASE("A full answer matches the documented completion golden", "[openai][response]") {
  std::vector<chat_event> const events{
      text_event("hi"),
      reasoning_event("think"),
      tool_start(0, "call_1", "read", R"({"path":"a.go"})"),
      stop_event("tool_calls", sample_usage()),
  };

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_answered);

  constexpr std::string_view expected =
      R"({"id":"chatcmpl-0123456789abcdef0123456789abcdef","object":"chat.completion",)"
      R"("created":1760000000,"model":"dmodel","choices":[{"index":0,"message":)"
      R"({"role":"assistant","content":"hi","reasoning_content":"think","tool_calls":[{)"
      R"("index":0,"id":"call_1","type":"function","function":{"name":"read",)"
      R"("arguments":"{\"path\":\"a.go\"}"}}]},"finish_reason":"tool_calls"}],)"
      R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15,)"
      R"("prompt_tokens_details":{"cached_tokens":2},)"
      R"("completion_tokens_details":{"reasoning_tokens":3}}})";
  CHECK(built->m_json == expected);
}

TEST_CASE("A plain text answer omits reasoning, tools and usage", "[openai][response]") {
  std::vector<chat_event> const events{text_event("Hel"), text_event("lo"), stop_event("")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_answered);

  constexpr std::string_view expected =
      R"({"id":"chatcmpl-0123456789abcdef0123456789abcdef","object":"chat.completion",)"
      R"("created":1760000000,"model":"dmodel","choices":[{"index":0,"message":)"
      R"({"role":"assistant","content":"Hello"},"finish_reason":"stop"}]})";
  CHECK(built->m_json == expected);
}

TEST_CASE("An answer without text still writes an empty content string", "[openai][response]") {
  std::vector<chat_event> const events{stop_event("")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find(R"("content":"")") != std::string::npos);
  CHECK(built->m_answered);
}

TEST_CASE("A run without stop still writes a full document and reports unanswered",
          "[openai][response]") {
  std::vector<chat_event> const events{text_event("partial"), tool_start(0, "", "read", "")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK_FALSE(built->m_answered);

  constexpr std::string_view expected =
      R"({"id":"chatcmpl-0123456789abcdef0123456789abcdef","object":"chat.completion",)"
      R"("created":1760000000,"model":"dmodel","choices":[{"index":0,"message":)"
      R"({"role":"assistant","content":"partial","tool_calls":[{"index":0,"id":")";
  CHECK(built->m_json.rfind(std::string{expected}, 0) == 0);
  CHECK(built->m_json.find(R"(","type":"function","function":{"name":"read","arguments":""}}]},)"
                           R"("finish_reason":"tool_calls"}]})") != std::string::npos);
}

TEST_CASE("A tool call without an index continues the sequence", "[openai][response]") {
  std::vector<chat_event> const events{
      tool_start(4, "call_a", "read", ""),
      tool_start(-1, "call_b", "write", ""),
      tool_start(-1, "call_c", "list", ""),
  };

  auto const built = build(events);
  REQUIRE(built.has_value());
  // Three starts, so the first generated index is 1 and the second 2.
  CHECK(built->m_json.find(R"({"index":0,"id":"call_b")") == std::string::npos);
  CHECK(built->m_json.find(R"({"index":1,"id":"call_b")") != std::string::npos);
  CHECK(built->m_json.find(R"({"index":2,"id":"call_c")") != std::string::npos);
  CHECK(built->m_json.find(R"({"index":4,"id":"call_a")") != std::string::npos);
}

TEST_CASE("Tool calls are sorted by index", "[openai][response]") {
  std::vector<chat_event> const events{
      tool_start(2, "call_c", "third", ""),
      tool_start(0, "call_a", "first", ""),
      tool_start(1, "call_b", "second", ""),
  };

  auto const built = build(events);
  REQUIRE(built.has_value());
  auto const first = built->m_json.find(R"("id":"call_a")");
  auto const second = built->m_json.find(R"("id":"call_b")");
  auto const third = built->m_json.find(R"("id":"call_c")");
  REQUIRE(first != std::string::npos);
  REQUIRE(second != std::string::npos);
  REQUIRE(third != std::string::npos);
  CHECK(first < second);
  CHECK(second < third);
}

TEST_CASE("A repeated tool index is not merged with the earlier call", "[openai][response]") {
  std::vector<chat_event> const events{
      tool_start(0, "call_a", "read", "{}"),
      tool_start(0, "call_b", "write", "{}"),
  };

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find(R"("id":"call_a")") != std::string::npos);
  CHECK(built->m_json.find(R"("id":"call_b")") != std::string::npos);
  CHECK(built->m_json.find(R"("name":"read")") != std::string::npos);
  CHECK(built->m_json.find(R"("name":"write")") != std::string::npos);
}

TEST_CASE("Tool arguments append to the matching call only", "[openai][response]") {
  std::vector<chat_event> const events{
      tool_start(0, "call_a", "read", "{"),
      tool_start(1, "call_b", "write", "{"),
      tool_arguments(1, R"("b":1)"),
      tool_arguments(1, "}"),
      tool_arguments(7, R"("orphan":true)"),
      tool_arguments(0, R"("a":1)"),
      tool_arguments(0, "}"),
  };

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find(R"("arguments":"{\"a\":1}")") != std::string::npos);
  CHECK(built->m_json.find(R"("arguments":"{\"b\":1}")") != std::string::npos);
  CHECK(built->m_json.find("orphan") == std::string::npos);
}

TEST_CASE("A tool call without an id gets a generated one", "[openai][response]") {
  std::vector<chat_event> const events{tool_start(0, "", "read", "")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  auto const marker = built->m_json.find(R"("id":"call_)");
  REQUIRE(marker != std::string::npos);
  auto const start = marker + std::string_view{R"("id":")"}.size();
  auto const value = built->m_json.substr(start, built->m_json.find('"', start) - start);
  CHECK(value.size() == 37);
  CHECK(value.substr(0, 5) == "call_");
  for (auto const digit : value.substr(5)) {
    CHECK(((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')));
  }
}

TEST_CASE("An error event becomes an error and drops the collected answer", "[openai][response]") {
  std::vector<chat_event> const events{text_event("half an answer"), error_event(401, "expired")};

  auto const built = build(events);
  REQUIRE_FALSE(built.has_value());
  CHECK(built.error().m_kind == qoderz::error_kind::auth);
  CHECK(built.error().m_http_status == 401);
  CHECK(built.error().m_message == "expired");
}

TEST_CASE("An error event after a stop is never seen", "[openai][response]") {
  std::vector<chat_event> const events{stop_event("stop"), error_event(503, "busy")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_answered);
  CHECK(built->m_json.find(R"("finish_reason":"stop")") != std::string::npos);
}

TEST_CASE("A statusless error event falls back to 502", "[openai][response]") {
  std::vector<chat_event> const events{error_event(0, "the model's tool call was cut short")};

  auto const built = build(events);
  REQUIRE_FALSE(built.has_value());
  CHECK(built.error().m_http_status == 502);
  CHECK(built.error().m_message == "the model's tool call was cut short");
}

TEST_CASE("Events after a stop are ignored", "[openai][response]") {
  std::vector<chat_event> const events{text_event("kept"), stop_event("stop"), text_event("lost")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find("lost") == std::string::npos);
  CHECK(built->m_answered);
}

TEST_CASE("Non-ASCII text stays raw and only JSON syntax is escaped", "[openai][response]") {
  std::vector<chat_event> const events{text_event("你好 <a>&'\""),
                                       text_event("back\\slash\ttab\nnewline"), stop_event("")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find("你好 <a>&'\\\"") != std::string::npos);
  CHECK(built->m_json.find("\\u4f60") == std::string::npos);
  CHECK(built->m_json.find("<a>&'") != std::string::npos);
  CHECK(built->m_json.find("back\\\\slash\\ttab\\nnewline") != std::string::npos);
}

TEST_CASE("The usage total falls back to the sum of prompt and completion", "[openai][response]") {
  std::vector<chat_event> const events{
      stop_event("stop", token_usage{.m_prompt = 7, .m_completion = 2, .m_total = 0})};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find(R"("usage":{"prompt_tokens":7,"completion_tokens":2,"total_tokens":9,)"
                           R"("prompt_tokens_details":{"cached_tokens":0},)"
                           R"("completion_tokens_details":{"reasoning_tokens":0}}})") !=
        std::string::npos);
}

TEST_CASE("An upstream finish reason is passed through verbatim", "[openai][response]") {
  std::vector<chat_event> const events{text_event("x"), stop_event("length")};

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK(built->m_json.find(R"("finish_reason":"length")") != std::string::npos);
}

TEST_CASE("An empty event list yields an unanswered empty answer", "[openai][response]") {
  std::vector<chat_event> const events;

  auto const built = build(events);
  REQUIRE(built.has_value());
  CHECK_FALSE(built->m_answered);
  CHECK(built->m_json.find(R"("content":""})") != std::string::npos);
  CHECK(built->m_json.find(R"("finish_reason":"stop")") != std::string::npos);
}

TEST_CASE("A generated completion id has the chatcmpl shape", "[openai][response]") {
  auto const first = qoderz::openai::make_completion_id();
  auto const second = qoderz::openai::make_completion_id();

  CHECK(first.size() == 41);
  CHECK(first.substr(0, 9) == "chatcmpl-");
  CHECK(first != second);
  for (auto const digit : first.substr(9)) {
    CHECK(((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')));
  }
}
