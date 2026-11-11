#include "protocol/sse/chat_event.hpp"
#include "qoder/chat/stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using qoderz::protocol::chat_event;
using qoderz::protocol::chat_event_kind;
using qoderz::qoder::chat::stream_parser;
using json = nlohmann::json;

constexpr std::string_view unnamed_tool =
    "the model's native tool call has no name; it is not sent on as a tool";

/// Feeds every line of `text` in order and then finishes, the way the client drains a response.
std::vector<chat_event> parse_all(std::string_view text) {
  stream_parser parser;
  std::vector<chat_event> events;
  std::size_t offset = 0;
  while (offset < text.size()) {
    auto const end = text.find('\n', offset);
    auto const line =
        end == std::string_view::npos ? text.substr(offset) : text.substr(offset, end - offset);
    for (auto& event : parser.on_line(line)) {
      events.push_back(std::move(event));
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  for (auto& event : parser.finish()) {
    events.push_back(std::move(event));
  }
  return events;
}

std::string joined_text(std::vector<chat_event> const& events) {
  std::string text;
  for (auto const& event : events) {
    if (event.m_kind == chat_event_kind::text) {
      text.append(event.m_text);
    }
  }
  return text;
}

std::vector<chat_event> of_kind(std::vector<chat_event> const& events, chat_event_kind kind) {
  std::vector<chat_event> selected;
  for (auto const& event : events) {
    if (event.m_kind == kind) {
      selected.push_back(event);
    }
  }
  return selected;
}

chat_event const& only_stop(std::vector<chat_event> const& events) {
  auto const stops = of_kind(events, chat_event_kind::stop);
  REQUIRE(stops.size() == 1);
  return stops.front();
}

/// One `data:` line carrying exactly this envelope document.
std::string line_of(json const& envelope) {
  return "data: " + envelope.dump() + "\n";
}

/// A frame whose body is the inner chunk text, the shape the upstream actually sends.
std::string frame_of(json const& chunk) {
  json envelope;
  envelope["statusCodeValue"] = 200;
  envelope["body"] = chunk.dump();
  return line_of(envelope);
}

json delta_chunk(json delta) {
  json chunk;
  chunk["choices"] = json::array({json{{"index", 0}, {"delta", std::move(delta)}}});
  return chunk;
}

json content_chunk(std::string const& text) {
  return delta_chunk(json{{"content", text}});
}

json tool_chunk(json call) {
  return delta_chunk(json{{"tool_calls", json::array({std::move(call)})}});
}

constexpr std::string_view hello_text = "Hello";
constexpr std::string_view world_text = " world";

json finish_chunk(std::string const& reason) {
  json choice;
  choice["index"] = 0;
  choice["delta"] = json::object();
  choice["finish_reason"] = reason;
  json chunk;
  chunk["choices"] = json::array({choice});
  return chunk;
}

json usage_chunk(int prompt, int completion, int total) {
  json usage;
  usage["prompt_tokens"] = prompt;
  usage["completion_tokens"] = completion;
  usage["total_tokens"] = total;
  usage["prompt_tokens_details"] = json{{"cached_tokens", 2}};
  usage["completion_tokens_details"] = json{{"reasoning_tokens", 3}};
  json chunk;
  chunk["choices"] = json::array();
  chunk["usage"] = std::move(usage);
  return chunk;
}

std::string done_line() {
  json envelope;
  envelope["statusCodeValue"] = 200;
  envelope["body"] = "[DONE]";
  return line_of(envelope);
}

} // namespace

TEST_CASE("the documented fixture yields two text events and a stop", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(content_chunk(std::string{hello_text})));
  stream.append(frame_of(content_chunk(std::string{world_text})));
  stream.append(frame_of(usage_chunk(10, 5, 15)));
  stream.append(done_line());

  auto const events = parse_all(stream);
  CHECK(joined_text(events) == "Hello world");
  auto const stop = only_stop(events);
  CHECK(stop.m_reason == "stop");
  REQUIRE(stop.m_usage.has_value());
  CHECK(stop.m_usage->m_prompt == 10);
  CHECK(stop.m_usage->m_completion == 5);
  CHECK(stop.m_usage->m_total == 15);
  CHECK(stop.m_usage->m_cached == 2);
  CHECK(stop.m_usage->m_reasoning == 3);
  CHECK(of_kind(events, chat_event_kind::error).empty());
}

TEST_CASE("the documented tool fixture starts a call, streams an argument and stops",
          "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(tool_chunk(
      json{{"index", 0}, {"id", "call_abc"}, {"function", json{{"arguments", R"({"path")"}}}})));
  stream.append(frame_of(tool_chunk(
      json{{"index", 0}, {"function", json{{"name", "read"}, {"arguments", R"(:"a.go"})"}}}})));
  stream.append(frame_of(tool_chunk(json{{"index", 0}, {"function", json{{"arguments", "}"}}}})));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 1);
  CHECK(starts.front().m_id == "call_abc");
  CHECK(starts.front().m_name == "read");
  CHECK(starts.front().m_arguments == R"({"path":"a.go"})");
  CHECK(starts.front().m_index == 0);

  auto const arguments = of_kind(events, chat_event_kind::tool_arguments);
  REQUIRE(arguments.size() == 1);
  CHECK(arguments.front().m_arguments == "}");
  CHECK(arguments.front().m_index == 0);

  CHECK(only_stop(events).m_reason == "tool_calls");
  CHECK(of_kind(events, chat_event_kind::error).empty());
}

TEST_CASE("a missing status code is a normal frame", "[qoder][chat][stream]") {
  json envelope;
  envelope["body"] = content_chunk("hi").dump();
  auto const events = parse_all(line_of(envelope));
  CHECK(joined_text(events) == "hi");
  CHECK(of_kind(events, chat_event_kind::error).empty());
  CHECK(only_stop(events).m_reason == "stop");
}

TEST_CASE("an unreadable status code is a failure", "[qoder][chat][stream]") {
  json envelope;
  envelope["statusCodeValue"] = "abc";
  envelope["body"] = R"({"code":"105","message":"no session"})";
  auto const events = parse_all(line_of(envelope));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  // The unreadable status becomes 0, and a status outside 400..599 is a 502 before the body counts.
  CHECK(errors.front().m_status == 502);
  CHECK(of_kind(events, chat_event_kind::stop).empty());
}

TEST_CASE("a status code other than 200 is a failure", "[qoder][chat][stream]") {
  struct expected {
    json m_status;
    int m_downstream;
  };
  std::vector<expected> const cases{
      {json(201), 502},   {json(0), 502},       {json(-1), 502},    {json(500), 400},
      {json("500"), 400}, {json(nullptr), 502}, {json("abc"), 502},
  };
  for (auto const& item : cases) {
    json envelope;
    envelope["statusCodeValue"] = item.m_status;
    envelope["body"] = R"({"code":"406","message":"bad"})";
    auto const events = parse_all(line_of(envelope));
    auto const errors = of_kind(events, chat_event_kind::error);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().m_status == item.m_downstream);
    CHECK(of_kind(events, chat_event_kind::stop).empty());
  }
}

TEST_CASE("the error text of a string body is used as it is", "[qoder][chat][stream]") {
  json envelope;
  envelope["statusCodeValue"] = 500;
  envelope["body"] = R"({"code":"105","message":"token expired"})";
  auto const events = parse_all(line_of(envelope));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 401);
  CHECK(errors.front().m_error == "token expired");
}

TEST_CASE("the error text of an object body is its own JSON", "[qoder][chat][stream]") {
  json envelope;
  envelope["statusCodeValue"] = 500;
  envelope["body"] = json{{"code", "110"}, {"message", "out of quota"}};
  auto const events = parse_all(line_of(envelope));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 429);
  CHECK(errors.front().m_error == "out of quota");
}

TEST_CASE("a failure without a body falls back to the whole envelope", "[qoder][chat][stream]") {
  json envelope;
  envelope["statusCodeValue"] = 404;
  auto const events = parse_all(line_of(envelope));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 404);
  CHECK(errors.front().m_error == "Qoder returned HTTP 404.");
}

TEST_CASE("a queue failure keeps its retry after", "[qoder][chat][stream]") {
  json envelope;
  envelope["statusCodeValue"] = 503;
  envelope["body"] = json{{"code", "10605"},
                          {"queue", json{{"isQueued", true}, {"retryAfterSeconds", 7}}},
                          {"message", "queued"}};
  auto const events = parse_all(line_of(envelope));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 503);
  CHECK(errors.front().m_retry_after == 7);
  CHECK(of_kind(events, chat_event_kind::stop).empty());
}

TEST_CASE("lines that are not an envelope are ignored", "[qoder][chat][stream]") {
  std::string stream;
  stream.append("data: [DONE]\n");
  stream.append("data:\n");
  stream.append("event: message\n");
  stream.append("id: 7\n");
  stream.append("retry: 100\n");
  stream.append("\n");
  stream.append("data\n");
  stream.append("data: \n");
  stream.append("data: not json\n");
  stream.append("data: [1,2,3]\n");
  stream.append("data: \"text\"\n");
  stream.append("data: 42\n");
  stream.append("data: null\n");
  stream.append(frame_of(content_chunk("Hello")));

  auto const events = parse_all(stream);
  CHECK(joined_text(events) == "Hello");
  CHECK(only_stop(events).m_reason == "stop");
}

TEST_CASE("an inner chunk that is not a usable object is skipped", "[qoder][chat][stream]") {
  for (auto const inner :
       {std::string{"[]"}, std::string{"\"x\""}, std::string{"7"}, std::string{"null"},
        std::string{R"({"choices":[]})"}, std::string{R"({"choices":"no"})"},
        std::string{R"({"choices":{}})"}, std::string{R"({"no_choices":1})"}, std::string{"{"}}) {
    json envelope;
    envelope["statusCodeValue"] = 200;
    envelope["body"] = inner;
    auto const events = parse_all(line_of(envelope));
    CHECK(joined_text(events).empty());
    CHECK(only_stop(events).m_reason == "stop");
  }
}

TEST_CASE("only the first choice is read", "[qoder][chat][stream]") {
  json chunk;
  chunk["choices"] = json::array({json{{"index", 0}, {"delta", json{{"content", "first"}}}},
                                  json{{"index", 1}, {"delta", json{{"content", "second"}}}}});
  CHECK(joined_text(parse_all(frame_of(chunk))) == "first");
}

TEST_CASE("reasoning arrives before text", "[qoder][chat][stream]") {
  auto const events = parse_all(
      frame_of(delta_chunk(json{{"reasoning_content", "thinking"}, {"content", "answer"}})));
  REQUIRE(events.size() >= 3);
  CHECK(events[0].m_kind == chat_event_kind::reasoning);
  CHECK(events[0].m_text == "thinking");
  CHECK(events[1].m_kind == chat_event_kind::text);
  CHECK(events[1].m_text == "answer");
  CHECK(events.back().m_kind == chat_event_kind::stop);
}

TEST_CASE("text tool calls split across chunks become one call", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(content_chunk("<tool_call><function=read>")));
  stream.append(frame_of(content_chunk("<parameter=path>a.go")));
  stream.append(frame_of(content_chunk("</parameter></function></tool_call>")));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 1);
  CHECK(starts.front().m_name == "read");
  CHECK(starts.front().m_arguments == R"({"path":"a.go"})");
  CHECK(only_stop(events).m_reason == "tool_calls");
}

TEST_CASE("text and native calls share one output index counter", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(
      frame_of(content_chunk(R"(<tool_call>{"name":"first","arguments":{}}</tool_call>)")));
  stream.append(
      frame_of(tool_chunk(json{{"index", 0},
                               {"id", "call_1"},
                               {"function", json{{"name", "second"}, {"arguments", "{}"}}}})));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 2);
  CHECK(starts[0].m_name == "first");
  CHECK(starts[0].m_index == 0);
  CHECK(starts[1].m_name == "second");
  CHECK(starts[1].m_index == 1);
  CHECK(only_stop(events).m_reason == "tool_calls");
}

TEST_CASE("a native call without a name at the end is an error, not a stop",
          "[qoder][chat][stream]") {
  auto const events = parse_all(frame_of(
      tool_chunk(json{{"index", 0}, {"id", "call_abc"}, {"function", json{{"arguments", "{}"}}}})));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 502);
  CHECK(errors.front().m_error == unnamed_tool);
  CHECK(of_kind(events, chat_event_kind::stop).empty());
}

TEST_CASE("the name arriving late flushes the text splitter first", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(
      frame_of(content_chunk(R"(<tool_call>{"name":"first","arguments":{}}</tool_call>)")));
  stream.append(frame_of(tool_chunk(json{{"index", 0}, {"function", json{{"arguments", "{}"}}}})));
  stream.append(frame_of(tool_chunk(json{{"index", 0}, {"function", json{{"name", "second"}}}})));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 2);
  CHECK(starts[0].m_name == "first");
  CHECK(starts[0].m_index == 0);
  CHECK(starts[1].m_name == "second");
  CHECK(starts[1].m_index == 1);
  CHECK(starts[1].m_arguments == "{}");
  CHECK(of_kind(events, chat_event_kind::error).empty());
}

TEST_CASE("tool calls that start out of order still form separate calls", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(
      tool_chunk(json{{"index", 1}, {"function", json{{"name", "b"}, {"arguments", "{}"}}}})));
  stream.append(frame_of(
      tool_chunk(json{{"index", 0}, {"function", json{{"name", "a"}, {"arguments", "{}"}}}})));
  stream.append(frame_of(tool_chunk(json{{"index", 1}, {"function", json{{"arguments", " "}}}})));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 2);
  CHECK(starts[0].m_name == "b");
  CHECK(starts[0].m_index == 0);
  CHECK(starts[1].m_name == "a");
  CHECK(starts[1].m_index == 1);

  auto const arguments = of_kind(events, chat_event_kind::tool_arguments);
  REQUIRE(arguments.size() == 1);
  CHECK(arguments.front().m_index == 0);
  CHECK(arguments.front().m_arguments == " ");
}

TEST_CASE("a repeated name is ignored and a late id does not rewrite the start",
          "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(
      tool_chunk(json{{"index", 0}, {"function", json{{"name", "read"}, {"arguments", "{}"}}}})));
  stream.append(
      frame_of(tool_chunk(json{{"index", 0},
                               {"id", "call_late"},
                               {"function", json{{"name", "other"}, {"arguments", " "}}}})));

  auto const events = parse_all(stream);
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 1);
  CHECK(starts.front().m_name == "read");
  CHECK(starts.front().m_id != "call_late");
  CHECK(starts.front().m_id.starts_with("call_"));
}

TEST_CASE("the top level name and arguments are accepted too", "[qoder][chat][stream]") {
  auto const events =
      parse_all(frame_of(tool_chunk(json{{"index", 0}, {"name", "read"}, {"arguments", "{}"}})));
  auto const starts = of_kind(events, chat_event_kind::tool_start);
  REQUIRE(starts.size() == 1);
  CHECK(starts.front().m_name == "read");
  CHECK(starts.front().m_arguments == "{}");
}

TEST_CASE("a truncated stream still ends with a stop", "[qoder][chat][stream]") {
  stream_parser parser;
  for (auto& event : parser.on_line("data: " + [] {
         json envelope;
         envelope["statusCodeValue"] = 200;
         envelope["body"] = content_chunk("hi").dump();
         return envelope.dump();
       }())) {
    (void)event;
  }
  auto const tail = parser.finish();
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().m_kind == chat_event_kind::stop);
  CHECK(tail.front().m_reason == "stop");
  CHECK(parser.done());
  CHECK(parser.finish().empty());
}

TEST_CASE("an empty stream still ends with a stop", "[qoder][chat][stream]") {
  auto const events = parse_all("");
  REQUIRE(events.size() == 1);
  CHECK(events.front().m_kind == chat_event_kind::stop);
  CHECK(events.front().m_reason == "stop");
  CHECK_FALSE(events.front().m_usage.has_value());
}

TEST_CASE("finish_reason length survives into the stop", "[qoder][chat][stream]") {
  auto const events =
      parse_all(frame_of(delta_chunk(json{{"content", "x"}})) + frame_of(finish_chunk("LENGTH")));
  CHECK(only_stop(events).m_reason == "length");
}

TEST_CASE("the last non-empty finish_reason wins", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(finish_chunk("length")));
  stream.append(frame_of(finish_chunk("stop")));
  CHECK(only_stop(parse_all(stream)).m_reason == "stop");
}

TEST_CASE("usage falls back through the documented fields", "[qoder][chat][stream]") {
  json usage;
  usage["prompt_tokens"] = 4;
  usage["completion_tokens"] = 6;
  usage["cached_tokens"] = 1;
  usage["completion_tokens_details"] = json{{"reasoning_tokens", 2}};
  json chunk;
  chunk["choices"] = json::array();
  chunk["usage"] = std::move(usage);

  auto const stop = only_stop(parse_all(frame_of(chunk)));
  REQUIRE(stop.m_usage.has_value());
  CHECK(stop.m_usage->m_total == 10);
  CHECK(stop.m_usage->m_cached == 1);
  CHECK(stop.m_usage->m_reasoning == 2);
}

TEST_CASE("a later usage frame overrides an earlier one", "[qoder][chat][stream]") {
  std::string stream;
  stream.append(frame_of(usage_chunk(1, 1, 2)));
  stream.append(frame_of(usage_chunk(3, 4, 7)));
  auto const stop = only_stop(parse_all(stream));
  REQUIRE(stop.m_usage.has_value());
  CHECK(stop.m_usage->m_prompt == 3);
  CHECK(stop.m_usage->m_total == 7);
}

TEST_CASE("[DONE] ends the stream and later lines are ignored", "[qoder][chat][stream]") {
  stream_parser parser;
  auto first = parser.on_line(done_line().substr(0, done_line().size() - 1));
  REQUIRE(first.size() == 1);
  CHECK(first.front().m_kind == chat_event_kind::stop);
  CHECK(parser.done());
  CHECK(parser.on_line(frame_of(content_chunk("late"))).empty());
  CHECK(parser.finish().empty());
}

TEST_CASE("a text tool call that cannot be read is an error without a stop",
          "[qoder][chat][stream]") {
  auto const events = parse_all(frame_of(content_chunk(R"(<tool_call>{"name":"x"}</tool_call>)")));
  auto const errors = of_kind(events, chat_event_kind::error);
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().m_status == 502);
  CHECK(of_kind(events, chat_event_kind::stop).empty());
}

TEST_CASE("content in a code fence stays text", "[qoder][chat][stream]") {
  auto const events =
      parse_all(frame_of(content_chunk("```\n<tool_call>{\"name\":\"x\"}</tool_call>\n```")));
  CHECK(joined_text(events).find("<tool_call>") != std::string::npos);
  CHECK(of_kind(events, chat_event_kind::error).empty());
  CHECK(only_stop(events).m_reason == "stop");
}
