#include "chat_test_support.hpp"
#include "server/error_writer.hpp"

namespace {

using namespace qoderz::test::chat;

/// Drives one streaming request into a sink whose SSE head fails, and checks that the handler
/// reports the failure without writing a frame or a body.
void check_failed_head(qoderz::error failure, error_kind expected) {
  fixture scope;
  auto handler = scope.make_handler();
  recording_sink sink;
  sink.m_begin_failure = std::move(failure);

  chat_http_request request;
  request.m_body = body_of("ultimate", true);
  request.m_session_id = "session-11";
  request.m_stream = true;
  auto outcome = run_awaitable(scope.m_io, handler.handle(request, sink));

  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == expected);
  CHECK_FALSE(sink.m_started);
  CHECK_FALSE(sink.m_streaming);
  CHECK_FALSE(sink.m_ended);
  CHECK(sink.m_frames.empty());
  CHECK(sink.m_json_writes == 0);
  CHECK(sink.m_json_status == 0);
}

} // namespace

TEST_CASE("a stream fails over from a quota answer and sticks to the second account",
          "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.chat("ada", 429, R"({"code":110,"message":"quota exhausted"})");
  auto done = ask(scope, "ultimate", true, "session-1");
  REQUIRE(done.m_outcome.has_value());
  CHECK(done.sink().m_streaming);
  CHECK(done.sink().m_ended);
  REQUIRE(done.sink().m_frames.size() == 5);
  CHECK(done.sink().m_frames.back() == "data: [DONE]\n\n");

  auto const role = chunk_of(done.sink().m_frames.at(0));
  CHECK(role.at("object") == "chat.completion.chunk");
  CHECK(role.at("model") == "ultimate");
  CHECK(role.at("choices").at(0).at("delta").at("role") == "assistant");
  CHECK(payload_of(done.sink().m_frames.at(1)).find(R"("content":"Hello")") != std::string::npos);

  CHECK(scope.chat_calls() == 2);
  CHECK(scope.m_health.is_cooling("qoder/ada"));
  // §6.3 keys the binding by the derived session id of §4.1, not by the raw header.
  auto const bound = scope.bound_account("session-1");
  REQUIRE(bound.has_value());
  CHECK(*bound == "qoder/bob");

  auto replay = ask(scope, "ultimate", true, "session-1");
  REQUIRE(replay.m_outcome.has_value());
  REQUIRE(scope.chat_calls() == 3);
  auto const first = scope.sent_body(1);
  auto const second = scope.sent_body(2);
  CHECK(first.at("business").at("stage") == "start");
  CHECK(second.at("business").at("stage") == "processing");
  CHECK(first.at("request_set_id") == second.at("request_set_id"));
  CHECK(first.at("request_id") != second.at("request_id"));
}

TEST_CASE("a non-streaming request fails over and writes exactly one body", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.chat("ada", 429, R"({"code":110,"message":"quota exhausted"})");
  auto done = ask(scope, "ultimate", false, "session-2");
  REQUIRE(done.m_outcome.has_value());
  CHECK(done.sink().m_started);
  CHECK_FALSE(done.sink().m_streaming);
  CHECK(done.sink().m_frames.empty());
  CHECK(done.sink().m_json_status == 200);
  CHECK(done.sink().m_json_writes == 1);
  auto const document = json::parse(done.sink().m_json_body, nullptr, false);
  CHECK(document.at("object") == "chat.completion");
  CHECK(document.at("choices").at(0).at("message").at("content") == "Hello world");
  CHECK(scope.chat_calls() == 2);
}

TEST_CASE("an error after the first frame never moves to another account", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.chat("ada", 200,
             frame(R"({"choices":[{"index":0,"delta":{"content":"part"}}]})") +
                 envelope(429, R"({"code":110,"message":"quota exhausted"})"));
  auto done = ask(scope, "ultimate", true, "session-3");
  REQUIRE(done.m_outcome.has_value());
  CHECK(scope.chat_calls() == 1);
  CHECK(done.sink().m_ended);
  REQUIRE(done.sink().m_frames.size() == 3);
  CHECK(payload_of(done.sink().m_frames.at(1)).find(R"("content":"part")") != std::string::npos);
  auto const failure = chunk_of(done.sink().m_frames.at(2));
  CHECK(failure.at("error").at("type") == "qoder_error");
  CHECK(failure.at("error").at("code") == 429);
  check_no_done_frame(done.sink().m_frames);
}

TEST_CASE("a transport failure after the first frame is one error frame", "[server][chat]") {
  fixture scope;
  scope.truncate_chat("ada");
  auto done = ask(scope, "ultimate", true, "session-4");
  REQUIRE(done.m_outcome.has_value());
  CHECK(scope.chat_calls() == 1);
  CHECK(done.sink().m_ended);
  REQUIRE_FALSE(done.sink().m_frames.empty());
  auto const failure = chunk_of(done.sink().m_frames.back());
  CHECK(failure.at("error").at("type") == "qoder_error");
  CHECK(failure.at("error").at("code") == 502);
  check_no_done_frame(done.sink().m_frames);
}

TEST_CASE("tool calls are framed while streaming and aggregated otherwise", "[server][chat]") {
  std::string stream =
      frame(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
            R"("function":{"name":"read","arguments":"{\"path\":\"a.go\"}"}}]}}]})");
  stream.append(frame(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,)"
                      R"("function":{"arguments":" more"}}]}}]})"));
  stream.append(done_frame());

  SECTION("streaming") {
    fixture scope;
    scope.chat("ada", 200, stream);
    auto done = ask(scope, "ultimate", true, "session-5");
    REQUIRE(done.m_outcome.has_value());
    REQUIRE(done.sink().m_frames.size() == 5);
    auto const start = chunk_of(done.sink().m_frames.at(1));
    CHECK(start.at("choices").at(0).at("delta").at("tool_calls").at(0).at("id") == "call_1");
    auto const appended = chunk_of(done.sink().m_frames.at(2));
    CHECK(appended.at("choices")
              .at(0)
              .at("delta")
              .at("tool_calls")
              .at(0)
              .at("function")
              .at("arguments") == " more");
    CHECK(chunk_of(done.sink().m_frames.at(3)).at("choices").at(0).at("finish_reason") ==
          "tool_calls");
    CHECK(done.sink().m_frames.back() == "data: [DONE]\n\n");
  }

  SECTION("non-streaming") {
    fixture scope;
    scope.chat("ada", 200, stream);
    auto done = ask(scope, "ultimate", false, "session-6");
    REQUIRE(done.m_outcome.has_value());
    auto const document = json::parse(done.sink().m_json_body, nullptr, false);
    auto const& call = document.at("choices").at(0).at("message").at("tool_calls").at(0);
    CHECK(call.at("id") == "call_1");
    CHECK(call.at("function").at("name") == "read");
    CHECK(call.at("function").at("arguments") == R"({"path":"a.go"} more)");
    CHECK(document.at("choices").at(0).at("finish_reason") == "tool_calls");
  }
}

TEST_CASE("an unreadable text tool call becomes one error frame", "[server][chat]") {
  fixture scope;
  std::string stream = frame(R"({"choices":[{"index":0,"delta":{"content":"Sure.\n"}}]})");
  stream.append(frame(R"({"choices":[{"index":0,"delta":{"content":"<tool_call>{\"name\":\"read\",)"
                      R"(\"arguments\":{\"path\":}}</tool_call>"}}]})"));
  scope.chat("ada", 200, stream);
  auto done = ask(scope, "ultimate", true, "session-7");
  REQUIRE(done.m_outcome.has_value());
  CHECK(done.sink().m_ended);
  REQUIRE(done.sink().m_frames.size() == 3);
  CHECK(chunk_of(done.sink().m_frames.at(2)).at("error").at("code") == 502);
  check_no_done_frame(done.sink().m_frames);
}

TEST_CASE("an empty event stream still answers both shapes", "[server][chat]") {
  SECTION("streaming ends with the done frame") {
    fixture scope;
    scope.chat("ada", 200, "");
    auto done = ask(scope, "ultimate", true, "session-8");
    REQUIRE(done.m_outcome.has_value());
    REQUIRE(done.sink().m_frames.size() == 3);
    CHECK(chunk_of(done.sink().m_frames.at(1)).at("choices").at(0).at("finish_reason") == "stop");
    CHECK(done.sink().m_frames.back() == "data: [DONE]\n\n");
    CHECK(done.sink().m_ended);
  }

  SECTION("non-streaming writes a complete empty completion") {
    fixture scope;
    scope.chat("ada", 200, "");
    auto done = ask(scope, "ultimate", false, "session-9");
    REQUIRE(done.m_outcome.has_value());
    CHECK(done.sink().m_json_status == 200);
    auto const document = json::parse(done.sink().m_json_body, nullptr, false);
    CHECK(document.at("choices").at(0).at("message").at("content") == "");
    CHECK(document.at("choices").at(0).at("finish_reason") == "stop");
    CHECK(document.find("usage") == document.end());
  }
}

TEST_CASE("a client that stops reading ends the request as canceled", "[server][chat]") {
  fixture scope;
  auto handler = scope.make_handler();
  recording_sink sink;
  sink.m_break_after = 1;

  chat_http_request request;
  request.m_body = body_of("ultimate", true);
  request.m_session_id = "session-10";
  request.m_stream = true;
  auto outcome = run_awaitable(scope.m_io, handler.handle(request, sink));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::canceled);
  CHECK(sink.m_frames.size() == 1);
  CHECK_FALSE(sink.m_ended);

  // Nothing was delivered, so the turn stays unanswered: the next request of the same session
  // opens a turn instead of continuing one. Marking it earlier would hide a half-written answer.
  auto replay = ask(scope, "ultimate", true, "session-10");
  REQUIRE(replay.m_outcome.has_value());
  REQUIRE(scope.chat_calls() == 2);
  CHECK(scope.sent_body(1).at("business").at("stage") == "start");
}

TEST_CASE("a non-streaming upstream error keeps a real status", "[server][chat]") {
  fixture scope;
  // The first event is the answer, the second one fails: §5.4 drops the buffer, so the account is
  // already fixed and the router still owns the status line.
  scope.chat("ada", 200,
             frame(R"({"choices":[{"index":0,"delta":{"content":"part"}}]})") +
                 envelope(429, R"({"code":110,"message":"quota exhausted"})"));
  auto const done = ask(scope, "ultimate", false, "session-12");

  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(done.m_outcome.error().m_kind == error_kind::quota);
  CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 429);
  CHECK(done.m_outcome.error().m_message == "quota exhausted");

  // No body was written, so the router can answer with the 429 instead of a 200 carrying an error
  // document, which is what a buffered non-streaming error relies on.
  CHECK_FALSE(done.sink().m_started);
  CHECK(done.sink().m_json_writes == 0);
  CHECK(done.sink().m_frames.empty());
  CHECK(scope.chat_calls() == 1);
}

TEST_CASE("a failed SSE head ends the request before any frame", "[server][chat]") {
  SECTION("the client went away") {
    check_failed_head(qoderz::make_error(error_kind::canceled, "the client went away"),
                      error_kind::canceled);
  }

  SECTION("the stream cannot be started") {
    check_failed_head(qoderz::make_error(error_kind::upstream, "the stream did not start"),
                      error_kind::upstream);
  }
}

TEST_CASE("a failed terminating chunk leaves the turn unanswered", "[server][chat]") {
  fixture scope;
  auto handler = scope.make_handler();
  recording_sink sink;
  sink.m_end_failure = qoderz::make_error(error_kind::canceled, "the client went away");

  chat_http_request request;
  request.m_body = body_of("ultimate", true);
  request.m_session_id = "session-13";
  request.m_stream = true;
  auto outcome = run_awaitable(scope.m_io, handler.handle(request, sink));

  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::canceled);
  CHECK(sink.m_streaming);
  CHECK_FALSE(sink.m_ended);
  REQUIRE_FALSE(sink.m_frames.empty());
  CHECK(sink.m_frames.back() == "data: [DONE]\n\n");

  // Every frame went out, but the terminating chunk did not: §5.5 marks the turn answered only
  // after a complete write, so the next request of this session opens a turn rather than continuing
  // one.
  auto replay = ask(scope, "ultimate", true, "session-13");
  REQUIRE(replay.m_outcome.has_value());
  REQUIRE(scope.chat_calls() == 2);
  CHECK(scope.sent_body(1).at("business").at("stage") == "start");
}
