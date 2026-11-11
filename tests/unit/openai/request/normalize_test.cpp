#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/normalize.hpp"
#include "openai/request/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::error_kind;
using qoderz::json;
using qoderz::openai::message_block;
using qoderz::openai::normalize_messages;
using qoderz::openai::normalize_result;
using qoderz::openai::normalized_message;
using qoderz::openai::read_text_content;

normalize_result normalize(std::string_view text) {
  auto result = normalize_messages(json::parse(text));
  REQUIRE(result.has_value());
  return *result;
}

std::vector<std::string> roles_of(std::vector<normalized_message> const& messages) {
  std::vector<std::string> roles;
  for (auto const& message : messages) {
    roles.push_back(message.m_role);
  }
  return roles;
}

std::vector<std::string> texts_of(normalized_message const& message) {
  std::vector<std::string> texts;
  for (auto const& block : message.m_blocks) {
    if (block.m_kind == message_block::kind::text) {
      texts.push_back(block.m_text);
    }
  }
  return texts;
}

std::vector<std::string> urls_of(normalized_message const& message) {
  std::vector<std::string> urls;
  for (auto const& block : message.m_blocks) {
    if (block.m_kind == message_block::kind::image_url) {
      urls.push_back(block.m_url);
    }
  }
  return urls;
}

} // namespace

TEST_CASE("A non-array messages value is rejected", "[openai][normalize]") {
  for (auto const* value : {R"({})", R"("hi")", "null", "7"}) {
    auto const result = normalize_messages(json::parse(value));
    INFO(value);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().m_kind == error_kind::bad_request);
    CHECK(result.error().m_message == "messages is required");
  }
}

TEST_CASE("Only the four dialog roles are kept", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"system","content":"dropped"},
    {"role":"developer","content":"dropped"},
    {"role":" user ","content":"kept"},
    {"role":"User","content":"dropped"},
    {"role":"user","content":"second"},
    {"role":"tool_result","content":"dropped"},
    {"role":"assistant","content":"kept"},
    {"content":"no role"},
    "not-an-object",
    42
  ])");

  CHECK(roles_of(result.m_messages) == std::vector<std::string>{"user", "user", "assistant"});
}

TEST_CASE("A string user content becomes one text block", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"user","content":" Hello "}])");

  REQUIRE(result.m_messages.size() == 1);
  CHECK(texts_of(result.m_messages[0]) == std::vector<std::string>{" Hello "});
  CHECK_FALSE(result.m_messages[0].m_array_content);
}

TEST_CASE("An array user content keeps text and images in order", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"user","content":[
    {"type":"text","text":"look"},
    {"type":"image_url","image_url":{"url":" https://example.test/a.png "}},
    {"type":"image_url","image_url":"https://example.test/b.png"},
    {"type":"input_audio","input_audio":{"data":"x"}},
    {"type":"file","file":{"file_id":"f"}},
    {"type":"image_url","image_url":{"url":"   "}},
    "bare string",
    17
  ]}])");

  REQUIRE(result.m_messages.size() == 1);
  auto const& message = result.m_messages[0];
  CHECK(texts_of(message) == std::vector<std::string>{"look", "bare string"});
  CHECK(urls_of(message) ==
        std::vector<std::string>{"https://example.test/a.png", "https://example.test/b.png"});
  CHECK(message.m_blocks.size() == 4);
  CHECK(message.m_array_content);
}

TEST_CASE("A missing or null user content still produces a user turn", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"user"},
    {"role":"user","content":null},
    {"role":"user","content":[]}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  for (auto const& message : result.m_messages) {
    CHECK(message.m_role == "user");
    CHECK(message.m_blocks.empty());
  }
  CHECK_FALSE(result.m_messages[0].m_array_content);
  CHECK(result.m_messages[2].m_array_content);
}

TEST_CASE("The first and last user text are tracked over raw indices", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"system","content":"ignored"},
    {"role":"user","content":"first"},
    {"role":"assistant","content":"reply"},
    {"role":"user","content":[{"type":"text","text":"second"},{"type":"text","text":"tail"}]}
  ])");

  CHECK(result.m_first_user_text == "first");
  CHECK(result.m_last_user_text == "second\ntail");
  CHECK(result.m_last_user_index == 3);
}

TEST_CASE("The last user index counts entries that were skipped", "[openai][normalize]") {
  auto const result = normalize(R"([
    "junk",
    {"role":"user","content":"hello"},
    {"role":"narrator","content":"skipped"},
    42
  ])");

  CHECK(result.m_last_user_index == 1);
}

TEST_CASE("A request without a user turn reports index minus one", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"assistant","content":"only"}])");

  CHECK(result.m_last_user_index == -1);
  CHECK(result.m_first_user_text.empty());
  CHECK(result.m_last_user_text.empty());
}

TEST_CASE("An assistant keeps only text content", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"assistant","content":[
    {"type":"text","text":"one"},
    {"type":"image_url","image_url":{"url":"https://example.test/a.png"}},
    {"type":"thinking","thinking":"hidden"},
    "two",
    {"type":"text","text":42}
  ]}])");

  REQUIRE(result.m_messages.size() == 1);
  CHECK(texts_of(result.m_messages[0]) == std::vector<std::string>{"one", "two"});
  CHECK(result.m_messages[0].m_tool_calls.empty());
}

TEST_CASE("Tool call arguments keep the client's text", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"assistant","content":null,"tool_calls":[
    {"id":"call_1","type":"function","function":{"name":" get_weather ","arguments":"{\"city\":\"Paris\"}"}},
    {"id":"call_2","type":"function","function":{"name":"bare"}},
    {"id":"call_3","type":"function","function":{"name":"typed","arguments":{"city":"Rome"}}}
  ]}])");

  REQUIRE(result.m_messages.size() == 1);
  auto const& calls = result.m_messages[0].m_tool_calls;
  REQUIRE(calls.size() == 3);
  CHECK(calls[0].m_id == "call_1");
  CHECK(calls[0].m_name == "get_weather");
  CHECK(calls[0].m_arguments == R"({"city":"Paris"})");
  CHECK(calls[1].m_arguments.empty());
  CHECK(calls[2].m_arguments == R"({"city":"Rome"})");
}

TEST_CASE("An assistant without a usable call list has no calls", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":"a","tool_calls":"nope"},
    {"role":"assistant","content":"b","tool_calls":[]},
    {"role":"assistant","content":"c","tool_calls":[3]}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  for (auto const& message : result.m_messages) {
    CHECK(message.m_tool_calls.empty());
  }
}

TEST_CASE("A missing call id is generated deterministically", "[openai][normalize]") {
  auto const body = R"([{"role":"assistant","content":null,"tool_calls":[
    {"type":"function","function":{"name":"get_weather","arguments":"{}"}}
  ]}])";
  auto const first = normalize(body);
  auto const second = normalize(body);

  REQUIRE(first.m_messages.size() == 1);
  auto const& id = first.m_messages[0].m_tool_calls[0].m_id;
  // SHA-256 of "0\00\0get_weather\0{}", first 32 hex.
  CHECK(id == "call_409644bce5ddb94ffb74636e27d2d2e2");
  CHECK(second.m_messages[0].m_tool_calls[0].m_id == id);
}

TEST_CASE("A dropped call still consumes its index", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"assistant","content":null,"tool_calls":[
    {"type":"function","function":{"name":"first","arguments":""}},
    "not-an-object",
    {"type":"function","function":{"name":"third"}}
  ]}])");

  auto const& calls = result.m_messages[0].m_tool_calls;
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].m_id == "call_5d7aa690ecf7fbc47d32181ee4d3b589");
  CHECK(calls[1].m_id != calls[0].m_id);
}

TEST_CASE("Tool calls allow only the immediately preceding assistant", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"user","content":"weather?"},
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"call_1","type":"function","function":{"name":"get_weather","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"call_1","content":"sunny"}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[2].m_role == "tool");
  CHECK(result.m_messages[2].m_tool_call_id == "call_1");
  CHECK(result.m_messages[2].m_text == "sunny");
}

TEST_CASE("A user turn clears the open calls", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"call_1","type":"function","function":{"name":"a","arguments":"{}"}}
    ]},
    {"role":"user","content":"never mind"},
    {"role":"tool","tool_call_id":"call_1","content":"late"}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[2].m_role == "user");
  CHECK(texts_of(result.m_messages[2]) == std::vector<std::string>{"late"});
}

TEST_CASE("An unmatched id never falls back to the earliest open call", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"one","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"other","content":"orphan"}
  ])");

  REQUIRE(result.m_messages.size() == 2);
  CHECK(result.m_messages[1].m_role == "user");
  CHECK(result.m_messages[1].m_blocks[0].m_text == "orphan");
}

TEST_CASE("A result without an id takes the earliest unmatched call", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"one","arguments":"{}"}},
      {"id":"c2","type":"function","function":{"name":"two","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c2","content":"second"},
    {"role":"tool","content":"first"}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[1].m_tool_call_id == "c2");
  CHECK(result.m_messages[2].m_tool_call_id == "c1");
}

TEST_CASE("A matched call cannot be reused", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"one","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":"first"},
    {"role":"tool","tool_call_id":"c1","content":"again"}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[1].m_role == "tool");
  CHECK(result.m_messages[2].m_role == "user");
}

TEST_CASE("An orphan result keeps whether its content was an array", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"tool","tool_call_id":"gone","content":[{"type":"text","text":"text"}]},
    {"role":"tool","tool_call_id":"gone","content":"plain"},
    {"role":"tool","tool_call_id":"gone","content":[{"type":"image_url","image_url":"  "}]}
  ])");

  REQUIRE(result.m_messages.size() == 2);
  CHECK(result.m_messages[0].m_array_content);
  CHECK_FALSE(result.m_messages[1].m_array_content);
}

TEST_CASE("A tool image moves into its own user turn with a note", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"user","content":"draw"},
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"call_1","type":"function","function":{"name":"draw","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"call_1","content":[
      {"type":"text","text":"rendered"},
      {"type":"image_url","image_url":{"url":"https://example.test/tool.png"}}
    ]},
    {"role":"user","content":"next"}
  ])");

  CHECK(roles_of(result.m_messages) ==
        std::vector<std::string>{"user", "assistant", "tool", "user", "user"});
  CHECK(result.m_messages[2].m_text ==
        "rendered\n\n[The tool returned an image; it follows in the next message.]");
  auto const& image_turn = result.m_messages[3];
  CHECK(image_turn.m_role == "user");
  CHECK(image_turn.m_array_content);
  REQUIRE(image_turn.m_blocks.size() == 2);
  CHECK(image_turn.m_blocks[0].m_text == "[From the result of call_1:]");
  CHECK(image_turn.m_blocks[1].m_url == "https://example.test/tool.png");
}

TEST_CASE("Several tool images produce one plural note", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"draw","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":[
      {"type":"image_url","image_url":"https://example.test/1.png"},
      {"type":"image_url","image_url":"https://example.test/2.png"}
    ]}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[1].m_text ==
        "[The tool returned 2 images; they follow in the next message.]");
  CHECK(result.m_messages[2].m_blocks.size() == 3);
}

TEST_CASE("A trailing tool image flushes at the end", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"draw","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":[
      {"type":"image_url","image_url":"https://example.test/1.png"}
    ]}
  ])");

  REQUIRE(result.m_messages.size() == 3);
  CHECK(result.m_messages[2].m_role == "user");
  CHECK(result.m_messages[2].m_blocks[0].m_text == "[From the result of c1:]");
  // The image turn is the last user message, so it is the one that carries the cache marker.
  CHECK(result.m_messages[2].m_blocks[1].m_cache_control);
}

TEST_CASE("An orphan image group is named after its call or tool", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"tool","tool_call_id":"lost","content":[
      {"type":"image_url","image_url":"https://example.test/1.png"}
    ]},
    {"role":"tool","content":[
      {"type":"image_url","image_url":"https://example.test/2.png"}
    ]},
    {"role":"user","content":"next"}
  ])");

  CHECK(roles_of(result.m_messages) == std::vector<std::string>{"user", "user", "user"});
  CHECK(result.m_messages[0].m_blocks[0].m_text == "[From the result of lost:]");
  CHECK(result.m_messages[1].m_blocks[0].m_text == "[From the result of tool:]");
  CHECK(result.m_messages[2].m_blocks[0].m_text == "next");
}

TEST_CASE("An empty orphan result is dropped", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"tool","tool_call_id":"gone","content":""},
    {"role":"tool","tool_call_id":"gone"}
  ])");

  CHECK(result.m_messages.empty());
}

TEST_CASE("Images waiting for a turn flush before the next user message", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"a","arguments":"{}"}},
      {"id":"c2","type":"function","function":{"name":"b","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":[
      {"type":"image_url","image_url":"https://example.test/1.png"}
    ]},
    {"role":"tool","tool_call_id":"c2","content":[
      {"type":"image_url","image_url":"https://example.test/2.png"}
    ]},
    {"role":"user","content":"thanks"}
  ])");

  CHECK(roles_of(result.m_messages) ==
        std::vector<std::string>{"assistant", "tool", "tool", "user", "user", "user"});
  CHECK(result.m_messages[3].m_blocks[0].m_text == "[From the result of c1:]");
  CHECK(result.m_messages[4].m_blocks[0].m_text == "[From the result of c2:]");
  CHECK(result.m_messages[5].m_blocks[0].m_text == "thanks");
}

TEST_CASE("Images waiting for a turn flush before the next assistant message",
          "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"assistant","content":null,"tool_calls":[
      {"id":"c1","type":"function","function":{"name":"a","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":[
      {"type":"image_url","image_url":"https://example.test/1.png"}
    ]},
    {"role":"assistant","content":"done"}
  ])");

  CHECK(roles_of(result.m_messages) ==
        std::vector<std::string>{"assistant", "tool", "user", "assistant"});
}

TEST_CASE("The last user block carries the cache marker", "[openai][normalize]") {
  auto const result = normalize(R"([{"role":"user","content":[
    {"type":"text","text":"one"},
    {"type":"text","text":"two"}
  ]}])");

  REQUIRE(result.m_messages.size() == 1);
  auto const& blocks = result.m_messages[0].m_blocks;
  REQUIRE(blocks.size() == 2);
  CHECK_FALSE(blocks[0].m_cache_control);
  CHECK(blocks[1].m_cache_control);
}

TEST_CASE("An empty trailing user turn gets no cache marker", "[openai][normalize]") {
  auto const result = normalize(R"([
    {"role":"user","content":"text"},
    {"role":"user","content":[]}
  ])");

  REQUIRE(result.m_messages.size() == 2);
  // The trailing turn has no block to mark, and the protocol only ever marks the last one.
  CHECK_FALSE(result.m_messages[0].m_blocks[0].m_cache_control);
  CHECK(result.m_messages[1].m_blocks.empty());
}

TEST_CASE("A trailing assistant or tool turn gets no cache marker", "[openai][normalize]") {
  auto const assistant_last = normalize(R"([
    {"role":"user","content":"hi"},
    {"role":"assistant","content":"answer"}
  ])");
  auto const tool_last = normalize(R"([
    {"role":"assistant","content":"calling","tool_calls":[
      {"id":"c1","type":"function","function":{"name":"a","arguments":"{}"}}
    ]},
    {"role":"tool","tool_call_id":"c1","content":"result"}
  ])");

  REQUIRE(assistant_last.m_messages.size() == 2);
  CHECK_FALSE(assistant_last.m_messages[0].m_blocks[0].m_cache_control);
  CHECK_FALSE(assistant_last.m_messages[1].m_blocks[0].m_cache_control);
  REQUIRE(tool_last.m_messages.size() == 2);
  CHECK_FALSE(tool_last.m_messages[0].m_blocks[0].m_cache_control);
  CHECK(tool_last.m_messages[1].m_text == "result");
}

TEST_CASE("Content text is read per the protocol rules", "[openai][normalize]") {
  CHECK(read_text_content(json::parse(R"("plain")")) == "plain");
  CHECK(read_text_content(json::parse(R"({"text":"object"})")) == "object");
  CHECK(read_text_content(json::parse(R"({"type":"input_text","text":"input"})")) == "input");
  CHECK(read_text_content(json::parse(R"({"type":"image_url","text":"wrong"})")).empty());
  CHECK(read_text_content(json::parse(R"({"text":42})")).empty());
  CHECK(read_text_content(json::parse(R"(["a","b"])")) == "a\nb");
  CHECK(read_text_content(json::parse(R"(["a",{"type":"text","text":"b"}])")) == "a\nb");
  CHECK(read_text_content(json::parse(R"([{"type":"image_url","image_url":"u"}])")).empty());
  CHECK(read_text_content(json::parse("null")).empty());
  CHECK(read_text_content(json::parse("7")).empty());
  // Text is never trimmed, so surrounding whitespace survives the round trip.
  CHECK(read_text_content(json::parse(R"("  spaced  ")")) == "  spaced  ");
}
