#include "openai/response/stream_frames.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::openai::response_identity;
using qoderz::openai::stream_translator;
using qoderz::protocol::chat_event;
using qoderz::protocol::chat_event_kind;
using qoderz::protocol::token_usage;

constexpr std::string_view fixed_head =
    R"(data: {"id":"chatcmpl-0123456789abcdef0123456789abcdef",)"
    R"("object":"chat.completion.chunk","created":1760000000,"model":"dmodel","choices":)";

constexpr std::string_view done_frame = "data: [DONE]\n\n";

response_identity fixture_identity() {
  return {.m_id = "chatcmpl-0123456789abcdef0123456789abcdef",
          .m_created = 1760000000,
          .m_model = "dmodel"};
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

constexpr token_usage sample_usage() {
  return {.m_prompt = 10, .m_completion = 5, .m_total = 15, .m_cached = 2, .m_reasoning = 3};
}

std::string frame_of(std::string_view choices_tail) {
  return std::string{fixed_head} + std::string{choices_tail};
}

} // namespace

TEST_CASE("A full stream matches the documented golden frames", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};
  std::vector<std::string> frames;

  auto const collect = [&frames](std::vector<std::string> produced) {
    frames.insert(frames.end(), produced.begin(), produced.end());
  };

  collect(translator.on_event(text_event("hi")));
  collect(translator.on_event(reasoning_event("think")));
  collect(translator.on_event(tool_start(0, "call_1", "read", R"({"path":"a.go"})")));
  collect(translator.on_event(tool_arguments(0, " more")));
  collect(translator.on_event(stop_event("tool_calls", sample_usage())));
  auto const tail = translator.finish();

  REQUIRE(frames.size() == 7);
  CHECK(tail.empty());
  CHECK(translator.answered());
  CHECK_FALSE(translator.failed());

  CHECK(
      frames.at(0) ==
      frame_of(R"([{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]})") +
          "\n\n");
  CHECK(frames.at(1) ==
        frame_of(R"([{"index":0,"delta":{"content":"hi"},"finish_reason":null}]})") + "\n\n");
  CHECK(frames.at(2) ==
        frame_of(R"([{"index":0,"delta":{"reasoning_content":"think"},"finish_reason":null}]})") +
            "\n\n");
  CHECK(frames.at(3) ==
        frame_of(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":)"
                 R"("function","function":{"name":"read","arguments":"{\"path\":\"a.go\"}"}}]},)"
                 R"("finish_reason":null}]})") +
            "\n\n");
  CHECK(frames.at(4) == frame_of(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"function":)"
                                 R"({"arguments":" more"}}]},"finish_reason":null}]})") +
                            "\n\n");
  CHECK(frames.at(5) ==
        frame_of(R"([{"index":0,"delta":{},"finish_reason":"tool_calls"}],)"
                 R"("usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15,)"
                 R"("prompt_tokens_details":{"cached_tokens":2},)"
                 R"("completion_tokens_details":{"reasoning_tokens":3}}})") +
            "\n\n");
  CHECK(frames.at(6) == done_frame);
}

TEST_CASE("The role frame is written once before the first event", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const first = translator.on_event(text_event("a"));
  auto const second = translator.on_event(text_event("b"));

  REQUIRE(first.size() == 2);
  CHECK(first.at(0).find(R"("delta":{"role":"assistant","content":""})") != std::string::npos);
  REQUIRE(second.size() == 1);
  CHECK(second.at(0).find(R"("role":"assistant")") == std::string::npos);
}

TEST_CASE("An error frame after content keeps the written frames and omits DONE",
          "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const before = translator.on_event(text_event("partial"));
  REQUIRE(before.size() == 2);

  auto const failed = translator.on_event(error_event(401, "expired"));
  REQUIRE(failed.size() == 1);
  CHECK(failed.at(0) == R"(data: {"error":{"message":"expired","type":"qoder_error","code":401}})"
                        "\n\n");
  CHECK(translator.failed());
  CHECK_FALSE(translator.answered());
  CHECK(translator.finish().empty());
}

TEST_CASE("An error as the first event writes no role frame", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const frames = translator.on_event(error_event(429, "quota"));

  REQUIRE(frames.size() == 1);
  CHECK(frames.at(0) == R"(data: {"error":{"message":"quota","type":"qoder_error","code":429}})"
                        "\n\n");
  CHECK(frames.at(0).find("role") == std::string::npos);
}

TEST_CASE("A statusless error frame falls back to 502", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const frames = translator.on_event(error_event(0, "unreadable tool call"));

  REQUIRE(frames.size() == 1);
  CHECK(frames.at(0) ==
        R"(data: {"error":{"message":"unreadable tool call","type":"qoder_error","code":502}})"
        "\n\n");
}

TEST_CASE("A stream without a stop ends with only DONE", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const produced = translator.on_event(text_event("hi"));
  REQUIRE(produced.size() == 2);

  CHECK(translator.finish() == done_frame);
  CHECK_FALSE(translator.answered());
  CHECK_FALSE(translator.failed());
}

TEST_CASE("An empty stream ends with only DONE", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  CHECK(translator.finish() == done_frame);
  CHECK_FALSE(translator.answered());
}

TEST_CASE("Finish is idempotent", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  CHECK(translator.finish() == done_frame);
  CHECK(translator.finish().empty());
  CHECK(translator.finish().empty());
}

TEST_CASE("Events after a stop produce no further frames", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const stopped = translator.on_event(stop_event("stop"));
  REQUIRE(stopped.size() == 3);
  CHECK(stopped.at(0).find(R"("delta":{"role":"assistant","content":""})") != std::string::npos);
  CHECK(stopped.at(1).find(R"("finish_reason":"stop")") != std::string::npos);
  CHECK(stopped.at(2) == done_frame);

  CHECK(translator.on_event(text_event("late")).empty());
  CHECK(translator.on_event(stop_event("stop")).empty());
  CHECK(translator.finish().empty());
}

TEST_CASE("A stop without usage carries no usage object", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const frames = translator.on_event(stop_event("length"));

  REQUIRE(frames.size() == 3);
  CHECK(frames.at(1) == frame_of(R"([{"index":0,"delta":{},"finish_reason":"length"}]})") + "\n\n");
  CHECK(frames.at(1).find("usage") == std::string::npos);
}

TEST_CASE("The identity is fixed across the whole stream", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};
  std::vector<std::string> frames;

  auto const collect = [&frames](std::vector<std::string> produced) {
    frames.insert(frames.end(), produced.begin(), produced.end());
  };

  collect(translator.on_event(text_event("a")));
  collect(translator.on_event(reasoning_event("b")));
  collect(translator.on_event(tool_start(-1, "call_x", "read", "")));
  collect(translator.on_event(stop_event("", sample_usage())));

  REQUIRE(frames.size() == 6);
  for (std::size_t index = 0; index + 1 < frames.size(); ++index) {
    INFO("frame " << index);
    CHECK(frames.at(index).rfind(std::string{fixed_head}, 0) == 0);
  }
  CHECK(frames.back() == done_frame);
}

TEST_CASE("A tool start without an index uses zero", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const frames = translator.on_event(tool_start(-1, "call_x", "read", "{}"));

  REQUIRE(frames.size() == 2);
  CHECK(frames.at(1).find(R"("tool_calls":[{"index":0,"id":"call_x")") != std::string::npos);
}

TEST_CASE("Non-ASCII text stays raw and only JSON syntax is escaped", "[openai][stream_frames]") {
  stream_translator translator{fixture_identity()};

  auto const frames = translator.on_event(text_event("你好 <a>&'"));

  REQUIRE(frames.size() == 2);
  CHECK(frames.at(1).find(R"("content":"你好 <a>&'")") != std::string::npos);
  CHECK(frames.at(1).find("\\u4f60") == std::string::npos);
  CHECK(frames.at(1).find("\\u003c") == std::string::npos);
  CHECK(frames.at(1).find("\\u0026") == std::string::npos);
}
