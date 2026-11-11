#include "protocol/sse/text_tool_splitter.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using qoderz::protocol::chat_event;
using qoderz::protocol::chat_event_kind;
using qoderz::protocol::text_tool_splitter;

constexpr std::string_view unreadable =
    "the model's tool call, written as text, can't be read; it is not sent on as the answer";
constexpr std::string_view cut_short =
    "the model's tool call, written as text, was cut short; it is not sent on as the answer";

struct outcome {
  std::string m_text;
  std::vector<std::pair<std::string, std::string>> m_tools;
  std::vector<std::string> m_errors;
  bool m_saw_tool = false;
};

outcome summarise(std::vector<chat_event> const& events, bool saw_tool) {
  outcome result;
  for (auto const& event : events) {
    switch (event.m_kind) {
    case chat_event_kind::text:
      result.m_text.append(event.m_text);
      break;
    case chat_event_kind::tool_start:
      result.m_tools.emplace_back(event.m_name, event.m_arguments);
      break;
    case chat_event_kind::error:
      CHECK(event.m_status == 502);
      result.m_errors.push_back(event.m_error);
      break;
    default:
      FAIL("unexpected event kind " << qoderz::protocol::to_string(event.m_kind));
      break;
    }
  }
  result.m_saw_tool = saw_tool;
  return result;
}

outcome split(std::vector<std::string> const& chunks) {
  text_tool_splitter splitter;
  std::vector<chat_event> events;
  for (auto const& chunk : chunks) {
    auto produced = splitter.feed(chunk);
    events.insert(events.end(), produced.begin(), produced.end());
  }
  auto tail = splitter.flush();
  events.insert(events.end(), tail.begin(), tail.end());
  return summarise(events, splitter.saw_tool());
}

outcome split(std::string const& text) {
  return split(std::vector<std::string>{text});
}

outcome split(std::initializer_list<std::string> chunks) {
  return split(std::vector<std::string>{chunks});
}

std::vector<std::string> chunked(std::string const& text, std::size_t size) {
  std::vector<std::string> chunks;
  for (std::size_t offset = 0; offset < text.size(); offset += size) {
    chunks.push_back(text.substr(offset, size));
  }
  return chunks;
}

constexpr std::size_t split_sizes[] = {1, 2, 3, 4, 5, 7};

std::size_t old_count(std::string const& text, std::string_view needle);
std::string remove_all(std::string const& text, std::string_view needle);

/// The pre-C++ implementation of the fence decision, kept verbatim as the oracle: odd fence count
/// quotes, otherwise an odd backtick run on the last line quotes.
bool old_is_quoted(std::string const& sent) {
  if (old_count(sent, "```") % 2 != 0) {
    return true;
  }
  auto const newline = sent.rfind('\n');
  auto const line = newline == std::string::npos ? sent : sent.substr(newline + 1);
  return old_count(remove_all(line, "```"), "`") % 2 != 0;
}

std::size_t old_count(std::string const& text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string remove_all(std::string const& text, std::string_view needle) {
  std::string result;
  std::size_t offset = 0;
  while (true) {
    auto const found = text.find(needle, offset);
    if (found == std::string::npos) {
      result.append(text, offset, std::string::npos);
      return result;
    }
    result.append(text, offset, found - offset);
    offset = found + needle.size();
  }
}

std::string emitted_text(std::vector<chat_event> const& events) {
  std::string text;
  for (auto const& event : events) {
    if (event.m_kind == chat_event_kind::text) {
      text.append(event.m_text);
    }
  }
  return text;
}

struct text_case {
  std::string m_input;
  std::string m_text;
  std::vector<std::pair<std::string, std::string>> m_tools;
  std::vector<std::string> m_errors;
};

// docs/protocol/openai-compat.md §9.4, verbatim.
std::vector<text_case> fixed_cases() {
  return {
      {"hello world", "hello world", {}, {}},
      {"Ends on <tool_c", "Ends on <tool_c", {}, {}},
      {"A block is <tool_call>name</tool_call> in prose.",
       "A block is <tool_call>name</tool_call> in prose.",
       {},
       {}},
      {"Use <tool_call>incomplete", "Use <tool_call>incomplete", {}, {}},
      {R"(Reading.
<tool_call>{"name":"Bash","arguments":{"command":"pwd"}}</tool_call>)",
       "Reading.\n",
       {{"Bash", R"({"command":"pwd"})"}},
       {}},
      {"<tool_call><function=Bash><parameter=command>pwd</parameter></function></tool_call>",
       "",
       {{"Bash", R"({"command":"pwd"})"}},
       {}},
      {"<tool_call><function=Bash><parameter=command>pwd</parameter>"
       "<parameter=timeout>30</parameter><parameter=force>true</parameter>"
       "<parameter=label>\"quoted\"</parameter><parameter=note>hello world</parameter>"
       "</function></tool_call>",
       "",
       {{"Bash",
         R"({"command":"pwd","timeout":30,"force":true,"label":"quoted","note":"hello world"})"}},
       {}},
      {"An "
       "example:\n```\n<tool_call>{\"name\":\"read\",\"arguments\":{\"path\":}}</tool_call>\n```\n",
       "An "
       "example:\n```\n<tool_call>{\"name\":\"read\",\"arguments\":{\"path\":}}</tool_call>\n```\n",
       {},
       {}},
      {"Write it as `<tool_call>{\"name\":` then the rest.",
       "Write it as `<tool_call>{\"name\":` then the rest.",
       {},
       {}},
      {R"(Sure.
<tool_call>{"name":"read","arguments":{"path":}}</tool_call>)",
       "Sure.\n",
       {},
       {std::string{unreadable}}},
      {R"(Reading.
<tool_call>{"name":"read","arguments":{"path":"a.go")",
       "Reading.\n",
       {},
       {std::string{cut_short}}},
      {R"(Reading.
<tool_call>{"name":"read","arguments":{"path":"a.go"}})",
       "Reading.\n",
       {{"read", R"({"path":"a.go"})"}},
       {}},
  };
}

} // namespace

TEST_CASE("plain text is emitted unchanged", "[protocol][splitter]") {
  auto const result = split({"hello ", "world"});
  CHECK(result.m_text == "hello world");
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
  CHECK_FALSE(result.m_saw_tool);
}

TEST_CASE("an empty chunk emits nothing", "[protocol][splitter]") {
  auto const result = split({"", "text", ""});
  CHECK(result.m_text == "text");
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
}

TEST_CASE("a partial open tag is held and flushed as text", "[protocol][splitter]") {
  auto const result = split("Ends on <tool_c");
  CHECK(result.m_text == "Ends on <tool_c");
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
}

TEST_CASE("prose that mentions a call shape stays text", "[protocol][splitter]") {
  auto const result = split("A block is <tool_call>name</tool_call> in prose.");
  CHECK(result.m_text == "A block is <tool_call>name</tool_call> in prose.");
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
}

TEST_CASE("an unfinished tag with plain text stays text", "[protocol][splitter]") {
  auto const result = split("Use <tool_call>incomplete");
  CHECK(result.m_text == "Use <tool_call>incomplete");
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
}

TEST_CASE("a json call yields a tool start", "[protocol][splitter]") {
  auto const result = split(
      "Reading.\n<tool_call>{\"name\":\"Bash\",\"arguments\":{\"command\":\"pwd\"}}</tool_call>");
  CHECK(result.m_text == "Reading.\n");
  REQUIRE(result.m_tools.size() == 1);
  CHECK(result.m_tools[0].first == "Bash");
  CHECK(result.m_tools[0].second == R"({"command":"pwd"})");
  CHECK(result.m_errors.empty());
  CHECK(result.m_saw_tool);
}

TEST_CASE("a json call split across many feeds still parses", "[protocol][splitter]") {
  constexpr std::string_view content =
      "Reading.\n<tool_call>{\"name\":\"Bash\",\"arguments\":{\"command\":\"pwd\"}}</tool_call>";
  auto const result = split(chunked(std::string{content}, 3));
  CHECK(result.m_text == "Reading.\n");
  REQUIRE(result.m_tools.size() == 1);
  CHECK(result.m_tools[0] ==
        std::make_pair(std::string{"Bash"}, std::string{R"({"command":"pwd"})"}));
}

TEST_CASE("an xml call yields a tool start", "[protocol][splitter]") {
  auto const result =
      split("<tool_call><function=Bash><parameter=command>pwd</parameter></function></tool_call>");
  CHECK(result.m_text.empty());
  REQUIRE(result.m_tools.size() == 1);
  CHECK(result.m_tools[0] ==
        std::make_pair(std::string{"Bash"}, std::string{R"({"command":"pwd"})"}));
  CHECK(result.m_saw_tool);
}

TEST_CASE("several calls are emitted in order around text", "[protocol][splitter]") {
  auto const result =
      split("one <tool_call>{\"name\":\"a\",\"arguments\":{}}</tool_call>"
            " two <tool_call><function=b><parameter=k>v</parameter></function></tool_call>"
            " three");
  CHECK(result.m_text == "one  two  three");
  REQUIRE(result.m_tools.size() == 2);
  CHECK(result.m_tools[0] == std::make_pair(std::string{"a"}, std::string{"{}"}));
  CHECK(result.m_tools[1] == std::make_pair(std::string{"b"}, std::string{R"({"k":"v"})"}));
  CHECK(result.m_errors.empty());
}

TEST_CASE("a call inside a fenced block stays text", "[protocol][splitter]") {
  constexpr std::string_view content =
      "An "
      "example:\n```\n<tool_call>{\"name\":\"read\",\"arguments\":{\"path\":}}</tool_call>\n```\n";
  auto const result = split(std::string{content});
  CHECK(result.m_text == content);
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
  CHECK_FALSE(result.m_saw_tool);
}

TEST_CASE("a call after an odd inline backtick stays text", "[protocol][splitter]") {
  constexpr std::string_view content = "Write it as `<tool_call>{\"name\":` then the rest.";
  auto const result = split(std::string{content});
  CHECK(result.m_text == content);
  CHECK(result.m_tools.empty());
  CHECK(result.m_errors.empty());
}

TEST_CASE("a closed but unreadable call becomes an error", "[protocol][splitter]") {
  auto const result =
      split("Sure.\n<tool_call>{\"name\":\"read\",\"arguments\":{\"path\":}}</tool_call>");
  CHECK(result.m_text == "Sure.\n");
  CHECK(result.m_tools.empty());
  REQUIRE(result.m_errors.size() == 1);
  CHECK(result.m_errors[0] == unreadable);
  CHECK_FALSE(result.m_saw_tool);
}

TEST_CASE("a call cut short becomes an error on flush", "[protocol][splitter]") {
  auto const result =
      split("Reading.\n<tool_call>{\"name\":\"read\",\"arguments\":{\"path\":\"a.go\"");
  CHECK(result.m_text == "Reading.\n");
  CHECK(result.m_tools.empty());
  REQUIRE(result.m_errors.size() == 1);
  CHECK(result.m_errors[0] == cut_short);
}

TEST_CASE("an xml call cut short becomes an error on flush", "[protocol][splitter]") {
  auto const result = split("<tool_call><function=read><parameter=path>a.go");
  REQUIRE(result.m_errors.size() == 1);
  CHECK(result.m_errors[0] == cut_short);
  CHECK(result.m_tools.empty());
}

TEST_CASE("an xml call without a name becomes an error", "[protocol][splitter]") {
  auto const result =
      split("<tool_call><function=><parameter=path>a.go</parameter></function></tool_call>");
  REQUIRE(result.m_errors.size() == 1);
  CHECK(result.m_errors[0] == unreadable);
  CHECK(result.m_tools.empty());
}

TEST_CASE("saw_tool stays false for text only", "[protocol][splitter]") {
  text_tool_splitter splitter;
  auto first = splitter.feed("plain ");
  auto second = splitter.feed("text");
  auto tail = splitter.flush();
  CHECK_FALSE(splitter.saw_tool());
  CHECK(emitted_text(first) == "plain ");
  CHECK(emitted_text(second) == "text");
  CHECK(tail.empty());
}

TEST_CASE("flushing twice does not repeat output", "[protocol][splitter]") {
  text_tool_splitter splitter;
  auto first = splitter.feed("trailing <tool_c");
  auto flushed = splitter.flush();
  auto after_first = first.size() + flushed.size();
  auto flushed_again = splitter.flush();

  CHECK(after_first == 2);
  CHECK(flushed_again.empty());
  CHECK(emitted_text(first) + emitted_text(flushed) == "trailing <tool_c");
}

TEST_CASE("parse_call reads both written forms", "[protocol][splitter]") {
  auto const json =
      text_tool_splitter::parse_call(R"({"name":"Bash","arguments":{"command":"pwd"}})");
  auto const xml = text_tool_splitter::parse_call(
      "<function=Bash><parameter=command>pwd</parameter></function>");

  REQUIRE(json.has_value());
  CHECK(json->m_name == "Bash");
  CHECK(json->m_arguments == R"({"command":"pwd"})");

  REQUIRE(xml.has_value());
  CHECK(xml->m_name == "Bash");
  CHECK(xml->m_arguments == R"({"command":"pwd"})");

  CHECK_FALSE(text_tool_splitter::parse_call("not a call").has_value());
  CHECK_FALSE(text_tool_splitter::parse_call(R"({"name":"Bash","arguments":[]})").has_value());
  CHECK_FALSE(text_tool_splitter::parse_call(R"({"name":"","arguments":{}})").has_value());
  CHECK_FALSE(text_tool_splitter::parse_call(R"({"name":3,"arguments":{}})").has_value());
}

TEST_CASE("tool events carry ordered ids and indexes", "[protocol][splitter]") {
  text_tool_splitter splitter;
  auto first = splitter.feed("<tool_call>{\"name\":\"a\",\"arguments\":{}}</tool_call>");
  auto second = splitter.feed("<tool_call>{\"name\":\"b\",\"arguments\":{}}</tool_call>");
  REQUIRE(first.size() == 1);
  REQUIRE(second.size() == 1);
  CHECK(first[0].m_index == 0);
  CHECK(second[0].m_index == 1);
  CHECK(first[0].m_id.starts_with("call_"));
  CHECK(first[0].m_id.size() == 5 + 32);
  CHECK(first[0].m_id != second[0].m_id);
}

TEST_CASE("the fixed case table matches at every split size", "[protocol][splitter]") {
  for (auto const& test : fixed_cases()) {
    CAPTURE(test.m_input);

    auto const whole = split(test.m_input);
    CHECK(whole.m_text == test.m_text);
    CHECK(whole.m_tools == test.m_tools);
    CHECK(whole.m_errors == test.m_errors);

    for (auto const size : split_sizes) {
      auto const chunked_result = split(chunked(test.m_input, size));
      CAPTURE(size);
      CHECK(chunked_result.m_text == whole.m_text);
      CHECK(chunked_result.m_tools == whole.m_tools);
      CHECK(chunked_result.m_errors == whole.m_errors);
      CHECK(chunked_result.m_saw_tool == whole.m_saw_tool);
    }
  }
}

namespace {

std::vector<std::string> quoting_texts() {
  return {"plain text",
          "a ` b",
          "a `` b",
          "a ``` b",
          "````",
          "`````` and `x`",
          "```\ninside\n```\nafter",
          "```\nopen only",
          "`open\n",
          "line ends with `\n",
          "\r\n```\r\nbody\r\n```\r\n",
          "tick run ````` ok",
          "\n\n```\n\n`\n",
          "a`b`c```d",
          "```x```y`",
          "`\n`\n",
          "````\n`\n````\n`",
          "``` ``` ``` `",
          "a \r\n ```\r\n b ```"};
}

} // namespace

TEST_CASE("the fence decision matches the whole string algorithm", "[protocol][splitter]") {
  CHECK_FALSE(text_tool_splitter{}.is_quoted());

  for (auto const& text : quoting_texts()) {
    CAPTURE(text);
    for (auto const size : split_sizes) {
      text_tool_splitter splitter;
      std::string emitted;
      for (auto const& chunk : chunked(text, size)) {
        auto const events = splitter.feed(chunk);
        emitted.append(emitted_text(events));
        CHECK(splitter.is_quoted() == old_is_quoted(emitted));
      }

      auto const tail = splitter.flush();
      emitted.append(emitted_text(tail));
      CHECK(splitter.is_quoted() == old_is_quoted(emitted));
      CHECK(old_is_quoted(text) == old_is_quoted(emitted));
    }
  }
}

TEST_CASE("the fence decision survives a deterministic sweep", "[protocol][splitter]") {
  std::vector<std::string> const atoms{"`", "`", "`", "a", "\n", "\r", " ", "(", "``", "\r\n"};
  std::uint32_t state = 20261109U;
  auto const next = [&state]() {
    state = state * 1664525U + 1013904223U;
    return state >> 16U;
  };

  for (int iteration = 0; iteration < 500; ++iteration) {
    std::string text;
    auto const atom_count = 1 + static_cast<int>(next() % 23U);
    for (int index = 0; index < atom_count; ++index) {
      text.append(atoms[next() % atoms.size()]);
    }

    text_tool_splitter splitter;
    std::string emitted;
    std::size_t offset = 0;
    while (offset < text.size()) {
      auto const remaining = text.size() - offset;
      auto const size = 1 + next() % std::min<std::size_t>(6, remaining);
      auto const events = splitter.feed(std::string_view{text}.substr(offset, size));
      emitted.append(emitted_text(events));
      offset += size;
      CHECK(splitter.is_quoted() == old_is_quoted(emitted));
    }

    auto const tail = splitter.flush();
    emitted.append(emitted_text(tail));
    CHECK(splitter.is_quoted() == old_is_quoted(emitted));
  }
}

TEST_CASE("the fence decision drives the unreadable call branch", "[protocol][splitter]") {
  constexpr std::string_view call = R"(<tool_call>{"name":"read","arguments":[]}</tool_call>)";

  for (auto const& prefix : quoting_texts()) {
    CAPTURE(prefix);
    auto const quoted = old_is_quoted(prefix);
    for (auto const size : split_sizes) {
      auto const result = split(chunked(prefix + std::string{call}, size));
      CHECK(result.m_tools.empty());
      CHECK_FALSE(result.m_saw_tool);
      if (quoted) {
        CHECK(result.m_text == prefix + std::string{call});
        CHECK(result.m_errors.empty());
      } else {
        CHECK(result.m_text == prefix);
        REQUIRE(result.m_errors.size() == 1);
        CHECK(result.m_errors[0] == unreadable);
      }
    }
  }
}

TEST_CASE("a re-emitted fence quotes the next call", "[protocol][splitter]") {
  constexpr std::string_view re_emitted = R"(<tool_call>`</tool_call>)";
  constexpr std::string_view call = R"(<tool_call>{"name":"read","arguments":[]}</tool_call>)";

  for (auto const size : split_sizes) {
    auto const result = split(chunked(std::string{re_emitted} + std::string{call}, size));
    CHECK(result.m_text == std::string{re_emitted} + std::string{call});
    CHECK(result.m_errors.empty());
    CHECK(result.m_tools.empty());
  }
}

namespace {

bool parses_as_json(std::string const& text) {
  try {
    auto const document = nlohmann::json::parse(text);
    return document.is_object();
  } catch (nlohmann::json::exception const&) {
    return false;
  }
}

std::string json_call_at_depth(std::size_t depth) {
  // Root object is level 1, the `arguments` object is level 2, every array adds one more.
  std::string text = R"({"name":"t","arguments":{"x":)";
  auto const nesting = depth >= 3 ? depth - 2 : 0;
  text.append(nesting, '[');
  text.append(nesting, ']');
  text.append("}}");
  return text;
}

} // namespace

TEST_CASE("an empty chunk yields no text event", "[protocol][splitter]") {
  text_tool_splitter splitter;
  auto const first = splitter.feed("");
  CHECK(first.empty());
  auto const second = splitter.feed("text");
  REQUIRE(second.size() == 1);
  CHECK(second[0].m_kind == chat_event_kind::text);
  CHECK(splitter.flush().empty());
}

TEST_CASE("invalid utf8 in a call does not throw", "[protocol][splitter]") {
  auto const bad_value = std::string{"<tool_call><function=t><parameter=a>"} +
                         std::string(1, '\xff') +
                         std::string{"</parameter></function></tool_call>"};
  auto const bad_key = std::string{"<tool_call><function=t><parameter="} + std::string(1, '\xfe') +
                       std::string{">v</parameter></function></tool_call>"};

  for (auto const& body : {bad_value, bad_key}) {
    text_tool_splitter splitter;
    std::vector<chat_event> events;
    CHECK_NOTHROW(events = splitter.feed(body));
    auto const tail = splitter.flush();
    events.insert(events.end(), tail.begin(), tail.end());

    auto const result = summarise(events, splitter.saw_tool());
    REQUIRE(result.m_tools.size() == 1);
    CHECK(result.m_errors.empty());
    CHECK(parses_as_json(result.m_tools[0].second));
    CHECK(result.m_tools[0].second.find("\xef\xbf\xbd") != std::string::npos);
  }
}

TEST_CASE("invalid utf8 in json arguments does not throw", "[protocol][splitter]") {
  std::string body = R"(<tool_call>{"name":"t","arguments":{"a":")";
  body.push_back('\xff');
  body.append(R"("}})");
  body.append("</tool_call>");

  text_tool_splitter splitter;
  std::vector<chat_event> events;
  CHECK_NOTHROW(events = splitter.feed(body));
  auto const tail = splitter.flush();
  events.insert(events.end(), tail.begin(), tail.end());

  auto const result = summarise(events, splitter.saw_tool());
  CHECK(result.m_tools.size() + result.m_errors.size() == 1);
  for (auto const& tool : result.m_tools) {
    CHECK(parses_as_json(tool.second));
  }
}

TEST_CASE("deeply nested json is rejected without recursion", "[protocol][splitter]") {
  CHECK(text_tool_splitter::parse_call(json_call_at_depth(63)).has_value());
  CHECK(text_tool_splitter::parse_call(json_call_at_depth(64)).has_value());
  CHECK_FALSE(text_tool_splitter::parse_call(json_call_at_depth(65)).has_value());
  CHECK_FALSE(text_tool_splitter::parse_call(json_call_at_depth(10000)).has_value());

  auto const too_deep = "<tool_call>" + json_call_at_depth(10000) + "</tool_call>";
  auto const result = split(too_deep);
  REQUIRE(result.m_errors.size() == 1);
  CHECK(result.m_errors[0] == unreadable);
  CHECK(result.m_tools.empty());
}

TEST_CASE("an over deep xml value falls back to a string", "[protocol][splitter]") {
  auto const deep_value = "<tool_call><function=t><parameter=a>" + std::string(10000, '[') +
                          std::string(10000, ']') + "</parameter></function></tool_call>";

  auto const result = split(deep_value);
  REQUIRE(result.m_tools.size() == 1);
  CHECK(parses_as_json(result.m_tools[0].second));
}
namespace {

/// A long, never-closed call body: exercising many feeds keeps an open call in flight.
std::string unclosed_call_text(std::size_t target) {
  std::string text = "Reading.\n<tool_call>";
  while (text.size() < target) {
    text.append(R"({"name":"read","arguments":{"path":")");
    text.append(96, 'a');
    text.push_back('"');
    text.append("},");
  }
  return text;
}

} // namespace

TEST_CASE("an unclosed call stays linear across feeds", "[protocol][splitter]") {
  constexpr std::size_t target = 1U << 20;
  constexpr std::size_t chunk_size = 8192;
  constexpr std::chrono::milliseconds budget{200};

  auto const text = unclosed_call_text(target);
  REQUIRE(text.size() >= target);

  auto const whole = split(text);

  auto const started = std::chrono::steady_clock::now();
  auto const chunked_result = split(chunked(text, chunk_size));
  auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  CHECK(chunked_result.m_text == whole.m_text);
  CHECK(chunked_result.m_tools == whole.m_tools);
  CHECK(chunked_result.m_errors == whole.m_errors);
  CHECK(chunked_result.m_saw_tool == whole.m_saw_tool);
  CHECK(elapsed < budget);
}
