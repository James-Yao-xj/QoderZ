#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/body.hpp"
#include "openai/request/types.hpp"
#include "qoder/catalog/model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using qoderz::json;
using ordered_json = nlohmann::ordered_json;
using qoderz::openai::build_qoder_body;
using qoderz::openai::message_block;
using qoderz::openai::normalized_message;
using qoderz::openai::request_context;
using qoderz::openai::tool_call;
using qoderz::openai::tool_spec;
using qoderz::qoder::catalog::model_info;

constexpr std::string_view system_prefix =
    "You are a Qoder agent. Use the instructions below and the tools available to you to assist "
    "the user.";

constexpr std::string_view required_line = "You must call an available function in this response.";

model_info fixture_model() {
  model_info model;
  model.m_key = "performance";
  model.m_name = "Performance";
  model.m_context = 1'000'000;
  model.m_default_window = 200'000;
  model.m_windows = {200'000, 1'000'000};
  model.m_raw_json = R"({"key":"performance"})";
  return model;
}

message_block text_of(std::string text) {
  message_block block;
  block.m_kind = message_block::kind::text;
  block.m_text = std::move(text);
  return block;
}

message_block image_of(std::string url) {
  message_block block;
  block.m_kind = message_block::kind::image_url;
  block.m_url = std::move(url);
  return block;
}

request_context basic_context() {
  request_context context;
  context.m_model = fixture_model();
  context.m_system_text = std::string{system_prefix};
  context.m_session_id = "0f8fad5b-d9cb-469f-a165-70867728950e";
  context.m_task = {.m_id = "11111111-2222-4333-8444-555555555555",
                    .m_begin_at_ms = 1'760'000'000'000,
                    .m_answered = false,
                    .m_name = "Hello"};
  context.m_request_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  context.m_enable_thinking = true;
  context.m_reasoning_effort = "medium";
  context.m_max_tokens = 32000;

  normalized_message user;
  user.m_role = "user";
  user.m_blocks.push_back(text_of("Hello"));
  context.m_messages.push_back(std::move(user));
  return context;
}

// Field order is part of the protocol, and `json` is a sorted map, so order has to be read back
// through `ordered_json`.
ordered_json parse_ordered(std::string const& text) {
  return ordered_json::parse(text);
}

std::vector<std::string> keys_of(ordered_json const& object) {
  std::vector<std::string> keys;
  for (auto const& [name, value] : object.items()) {
    (void)value;
    keys.push_back(name);
  }
  return keys;
}

std::string text_of_message(json const& message) {
  return message.at("content").at(0).at("text").get<std::string>();
}

} // namespace

TEST_CASE("The basic request matches the documented golden", "[openai][body]") {
  auto const built = build_qoder_body(basic_context());
  REQUIRE(built.has_value());
  auto const body = json::parse(built->m_json);

  CHECK(text_of_message(body.at("messages").at(0)) == system_prefix);
  CHECK(body.at("system").at(0).at("text") == system_prefix);
  CHECK(body.at("messages").at(1).at("role") == "user");
  CHECK(text_of_message(body.at("messages").at(1)) == "Hello");

  auto const parameters = body.at("parameters");
  CHECK(parameters.at("enable_thinking").get<bool>());
  CHECK(parameters.at("max_tokens").get<int>() == 32000);
  CHECK(parameters.at("reasoning_effort") == "medium");
  CHECK(parameters.at("context_length").get<int>() == 200'000);
  CHECK(built->m_context_length == 200'000);

  CHECK_FALSE(body.contains("temperature"));
  CHECK(body.at("request_id") == body.at("chat_record_id"));
  CHECK(body.at("request_id") == "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
  CHECK(body.at("request_set_id") == "11111111-2222-4333-8444-555555555555");
  CHECK(body.at("session_id") == "0f8fad5b-d9cb-469f-a165-70867728950e");
  CHECK(body.at("business").at("id") == body.at("request_set_id"));
  CHECK(body.at("business").at("name") == "Hello");
  CHECK(body.at("business").at("stage") == "start");
  CHECK(body.at("business").at("begin_at").get<std::int64_t>() == 1'760'000'000'000);
  CHECK_FALSE(body.contains("tools"));
}

TEST_CASE("The envelope fields are written in the documented order", "[openai][body]") {
  auto const built = build_qoder_body(basic_context());
  REQUIRE(built.has_value());

  CHECK(keys_of(parse_ordered(built->m_json)) ==
        std::vector<std::string>{"request_id", "request_set_id", "chat_record_id", "session_id",
                                 "business", "agent_id", "task_id", "session_type", "model_config",
                                 "system", "messages", "parameters"});
  CHECK(keys_of(parse_ordered(built->m_json).at("business")) ==
        std::vector<std::string>{"product", "version", "type", "id", "name", "begin_at", "stage"});
  CHECK(keys_of(parse_ordered(built->m_json).at("parameters")) ==
        std::vector<std::string>{"enable_thinking", "max_tokens", "reasoning_effort",
                                 "context_length"});
}

TEST_CASE("The fixed envelope constants are the documented ones", "[openai][body]") {
  auto const built = build_qoder_body(basic_context());
  REQUIRE(built.has_value());
  auto const body = json::parse(built->m_json);

  CHECK(body.at("business").at("product") == "app");
  CHECK(body.at("business").at("version") == "1.1.69");
  CHECK(body.at("business").at("type") == "agent");
  CHECK(body.at("agent_id") == "agent_common");
  CHECK(body.at("task_id") == "common");
  CHECK(body.at("session_type") == "app");
  CHECK(body.at("model_config").at("key") == "performance");
}

TEST_CASE("An answered task reports the processing stage", "[openai][body]") {
  auto context = basic_context();
  context.m_task.m_answered = true;

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  CHECK(json::parse(built->m_json).at("business").at("stage") == "processing");
}

TEST_CASE("An unreadable model config falls back to an empty object", "[openai][body]") {
  for (auto const* raw : {"", "[]", "not json", "42"}) {
    auto context = basic_context();
    context.m_model.m_raw_json = raw;

    auto const built = build_qoder_body(context);
    REQUIRE(built.has_value());
    INFO(raw);
    CHECK(json::parse(built->m_json).at("model_config").is_object());
    CHECK(json::parse(built->m_json).at("model_config").empty());
  }
}

TEST_CASE("The model config is embedded verbatim", "[openai][body]") {
  auto context = basic_context();
  context.m_model.m_raw_json = R"({"key":"performance","x-vendor":7,"nested":{"a":[1,2]}})";

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  auto const config = json::parse(built->m_json).at("model_config");
  CHECK(config.at("x-vendor").get<int>() == 7);
  CHECK(config.at("nested").at("a").at(1).get<int>() == 2);
}

TEST_CASE("An array user content is written as content and contents", "[openai][body]") {
  auto context = basic_context();
  context.m_messages.clear();
  normalized_message user;
  user.m_role = "user";
  user.m_array_content = true;
  user.m_blocks.push_back(text_of("one"));
  user.m_blocks.push_back(image_of("https://example.test/a.png"));
  context.m_messages.push_back(std::move(user));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const written = json::parse(built->m_json).at("messages").at(1);

  CHECK(written.at("content").dump() == written.at("contents").dump());
  CHECK(written.at("content").at(1).at("type") == "image_url");
  CHECK(written.at("content").at(1).at("image_url").at("url") == "https://example.test/a.png");
}

TEST_CASE("A string user content has no contents field", "[openai][body]") {
  auto const built = build_qoder_body(basic_context());
  REQUIRE(built.has_value());

  CHECK_FALSE(json::parse(built->m_json).at("messages").at(1).contains("contents"));
}

TEST_CASE("The cache marker becomes an ephemeral cache_control object", "[openai][body]") {
  auto context = basic_context();
  context.m_messages.clear();
  normalized_message user;
  user.m_role = "user";
  user.m_array_content = true;
  user.m_blocks.push_back(text_of("one"));
  user.m_blocks.push_back(text_of("two"));
  user.m_blocks.back().m_cache_control = true;
  context.m_messages.push_back(std::move(user));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const content = json::parse(built->m_json).at("messages").at(1).at("content");

  CHECK(content.at(1).at("cache_control").at("type") == "ephemeral");
  CHECK_FALSE(content.at(0).contains("cache_control"));
}

TEST_CASE("A tool message is a plain string", "[openai][body]") {
  auto context = basic_context();
  normalized_message tool;
  tool.m_role = "tool";
  tool.m_tool_call_id = "call_1";
  tool.m_text = "sunny";
  context.m_messages.push_back(std::move(tool));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const written = json::parse(built->m_json).at("messages").at(2);

  CHECK(written.at("role") == "tool");
  CHECK(written.at("tool_call_id") == "call_1");
  CHECK(written.at("content").is_string());
  CHECK(written.at("content") == "sunny");
}

TEST_CASE("An assistant with calls writes the documented call shape", "[openai][body]") {
  auto context = basic_context();
  normalized_message assistant;
  assistant.m_role = "assistant";
  assistant.m_blocks.push_back(text_of("checking"));
  assistant.m_tool_calls.push_back(
      tool_call{.m_id = "call_1", .m_name = "get_weather", .m_arguments = R"({"city":"Paris"})"});
  context.m_messages.push_back(std::move(assistant));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const written = json::parse(built->m_json).at("messages").at(2);
  auto const call = written.at("tool_calls").at(0);

  CHECK(written.at("role") == "assistant");
  CHECK(call.at("id") == "call_1");
  CHECK(call.at("type") == "function");
  CHECK(call.at("function").at("name") == "get_weather");
  CHECK(call.at("function").at("arguments") == R"({"city":"Paris"})");
  CHECK_FALSE(written.contains("contents"));
}

TEST_CASE("An assistant without calls writes none", "[openai][body]") {
  auto context = basic_context();
  normalized_message assistant;
  assistant.m_role = "assistant";
  assistant.m_blocks.push_back(text_of("answer"));
  context.m_messages.push_back(std::move(assistant));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  CHECK_FALSE(json::parse(built->m_json).at("messages").at(2).contains("tool_calls"));
}

TEST_CASE("Tools are written only when they are included", "[openai][body]") {
  auto context = basic_context();
  context.m_tools.push_back(tool_spec{.m_name = "get_weather",
                                      .m_description = "Weather lookups",
                                      .m_parameters_json = R"({"type":"object"})"});

  auto const without = build_qoder_body(context);
  REQUIRE(without.has_value());
  CHECK_FALSE(json::parse(without->m_json).contains("tools"));

  context.m_include_tools = true;
  auto const with = build_qoder_body(context);
  REQUIRE(with.has_value());
  auto const tools = json::parse(with->m_json).at("tools");

  REQUIRE(tools.size() == 1);
  CHECK(tools.at(0).at("type") == "function");
  CHECK(tools.at(0).at("function").at("name") == "get_weather");
  CHECK(tools.at(0).at("function").at("description") == "Weather lookups");
  CHECK(tools.at(0).at("function").at("parameters").at("type") == "object");
  CHECK(keys_of(parse_ordered(with->m_json)) ==
        std::vector<std::string>{"request_id", "request_set_id", "chat_record_id", "session_id",
                                 "business", "agent_id", "task_id", "session_type", "model_config",
                                 "system", "messages", "tools", "parameters"});
}

TEST_CASE("An empty thinking effort is omitted from the parameters", "[openai][body]") {
  auto context = basic_context();
  context.m_reasoning_effort.clear();
  context.m_enable_thinking = false;

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const parameters = json::parse(built->m_json).at("parameters");

  CHECK_FALSE(parameters.contains("reasoning_effort"));
  CHECK_FALSE(parameters.at("enable_thinking").get<bool>());
}

TEST_CASE("A larger prompt selects a larger context window", "[openai][body]") {
  auto context = basic_context();
  context.m_messages.clear();
  normalized_message user;
  user.m_role = "user";
  user.m_blocks.push_back(text_of(std::string(600'000, 'x')));
  context.m_messages.push_back(std::move(user));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  CHECK(built->m_context_length == 1'000'000);
  CHECK(json::parse(built->m_json).at("parameters").at("context_length").get<int>() == 1'000'000);
}

TEST_CASE("The window count uses the serialised bytes", "[openai][body]") {
  // A model whose only window is unreachable by the default one: the reported window must follow
  // the escaped byte count, not the character count.
  auto context = basic_context();
  context.m_model.m_windows = {10'000, 20'000};
  context.m_model.m_default_window = 10'000;
  context.m_model.m_context = 20'000;
  context.m_messages.clear();
  normalized_message user;
  user.m_role = "user";
  user.m_blocks.push_back(text_of(std::string(60'000, '"')));
  context.m_messages.push_back(std::move(user));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  // Every quote becomes `\"`, so 60 000 characters are at least 120 000 bytes: no window fits and
  // the largest one is reported.
  CHECK(built->m_context_length == 20'000);
}

TEST_CASE("A zero context length is not written", "[openai][body]") {
  auto context = basic_context();
  context.m_model.m_windows.clear();
  context.m_model.m_context = 0;
  context.m_model.m_default_window = 0;

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  CHECK(built->m_context_length == 0);
  CHECK_FALSE(json::parse(built->m_json).at("parameters").contains("context_length"));
}

TEST_CASE("Non-ASCII text stays raw UTF-8", "[openai][body]") {
  auto context = basic_context();
  context.m_messages.clear();
  normalized_message user;
  user.m_role = "user";
  user.m_blocks.push_back(text_of("你好 <world> & 'quoted'"));
  context.m_messages.push_back(std::move(user));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());

  CHECK(built->m_json.find("你好") != std::string::npos);
  CHECK(built->m_json.find("\\u") == std::string::npos);
  CHECK(built->m_json.find("<world>") != std::string::npos);
  CHECK(text_of_message(json::parse(built->m_json).at("messages").at(1)) ==
        "你好 <world> & 'quoted'");
}

TEST_CASE("Streamed request serialization preserves compact JSON bytes", "[openai][body]") {
  auto context = basic_context();
  context.m_system_text = "system 你好\n\"quoted\"";
  context.m_model.m_raw_json = R"({"key":"performance","weight":0.5,"enabled":true})";
  context.m_include_tools = true;
  context.m_tools.push_back(tool_spec{
      .m_name = "inspect",
      .m_description = "schema 猫\n\"quoted\"",
      .m_parameters_json = R"({"type":"object","properties":{"路径":{"type":"string"}}})"});

  std::string text{"你好 😺 \"quoted\"\\path\n\t\r\b\f"};
  text.push_back('\0');
  text.push_back('\x01');
  auto& user = context.m_messages.front();
  user.m_array_content = true;
  user.m_blocks = {text_of(text), image_of("https://example.test/猫.png")};
  user.m_blocks.back().m_cache_control = true;

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const body = parse_ordered(built->m_json);
  auto const& written = body.at("messages").at(1);

  CHECK(built->m_json == body.dump());
  CHECK(built->m_json.find(R"(你好 😺 \"quoted\"\\path\n\t\r\b\f\u0000\u0001)") !=
        std::string::npos);
  CHECK(built->m_json.find("\\u4f60") == std::string::npos);
  CHECK(written.at("content") == written.at("contents"));
  CHECK(written.at("content").at(0).at("text").get<std::string>() == text);
  CHECK(written.at("content").at(1).at("cache_control").at("type") == "ephemeral");
  CHECK(body.at("tools").at(0).at("function").at("description") == "schema 猫\n\"quoted\"");
  CHECK(body.at("model_config").at("weight").get<double>() == 0.5);
}

TEST_CASE("The context window counts escaped UTF-8 bytes and duplicated contents",
          "[openai][body]") {
  auto context = basic_context();
  context.m_max_tokens = 1;
  context.m_system_text = "system 猫\n";
  auto& user = context.m_messages.front();
  user.m_array_content = true;
  user.m_blocks = {text_of("你好 😺 \"quoted\"\\path\n\t"), image_of("data:image/png;base64,AA==")};
  context.m_tools.push_back(
      tool_spec{.m_name = "inspect", .m_description = "schema 猫\n", .m_parameters_json = "true"});

  for (auto const include_tools : {false, true}) {
    context.m_include_tools = include_tools;
    auto const reference = build_qoder_body(context);
    REQUIRE(reference.has_value());
    auto const body = parse_ordered(reference->m_json);
    auto const messages_bytes = body.at("messages").dump().size();
    auto const tools_bytes = include_tools ? body.at("tools").dump().size() : 2;
    auto const needed = static_cast<int>((messages_bytes + tools_bytes + 11 + 2) / 3) + 1;
    context.m_model.m_windows = {needed - 1, needed, needed + 1};
    context.m_model.m_default_window = needed - 1;
    context.m_model.m_context = needed + 1;

    auto const built = build_qoder_body(context);
    REQUIRE(built.has_value());
    INFO(include_tools);
    CHECK(built->m_context_length == needed);
    CHECK(json::parse(built->m_json).at("parameters").at("context_length") == needed);
  }
}

TEST_CASE("Streamed request serialization keeps strict UTF-8 validation", "[openai][body]") {
  auto context = basic_context();
  context.m_messages.front().m_blocks.front().m_text = std::string(1, static_cast<char>(0xFF));

  CHECK_THROWS_AS(build_qoder_body(context), json::type_error);
}

TEST_CASE("A request with tools, images, cache and history round-trips", "[openai][body]") {
  request_context context;
  context.m_model = fixture_model();
  context.m_system_text = std::string{system_prefix} + "\n" + std::string{required_line};
  context.m_session_id = "0f8fad5b-d9cb-469f-a165-70867728950e";
  context.m_task = {.m_id = "11111111-2222-4333-8444-555555555555",
                    .m_begin_at_ms = 1'760'000'000'000,
                    .m_answered = true,
                    .m_name = "draw a cat"};
  context.m_request_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  context.m_max_tokens = 1234;
  context.m_include_tools = true;
  context.m_tools.push_back(tool_spec{
      .m_name = "draw", .m_description = "Draw", .m_parameters_json = R"({"type":"object"})"});

  normalized_message user;
  user.m_role = "user";
  user.m_array_content = true;
  user.m_blocks.push_back(text_of("draw a cat"));
  user.m_blocks.push_back(image_of("https://example.test/cat.png"));
  user.m_blocks.back().m_cache_control = true;
  context.m_messages.push_back(std::move(user));

  normalized_message assistant;
  assistant.m_role = "assistant";
  assistant.m_tool_calls.push_back(
      tool_call{.m_id = "call_1", .m_name = "draw", .m_arguments = R"({"prompt":"cat"})"});
  context.m_messages.push_back(std::move(assistant));

  normalized_message tool;
  tool.m_role = "tool";
  tool.m_tool_call_id = "call_1";
  tool.m_text = "rendered";
  context.m_messages.push_back(std::move(tool));

  normalized_message image_turn;
  image_turn.m_role = "user";
  image_turn.m_array_content = true;
  image_turn.m_blocks.push_back(text_of("[From the result of call_1:]"));
  image_turn.m_blocks.push_back(image_of("https://example.test/out.png"));
  context.m_messages.push_back(std::move(image_turn));

  auto const built = build_qoder_body(context);
  REQUIRE(built.has_value());
  auto const body = parse_ordered(built->m_json);
  auto const messages = body.at("messages");

  REQUIRE(messages.size() == 5);
  CHECK(messages.at(0).at("role") == "system");
  CHECK(messages.at(0).at("content").at(0).at("text") == body.at("system").at(0).at("text"));

  auto const first_user = messages.at(1);
  CHECK(first_user.at("content").dump() == first_user.at("contents").dump());
  CHECK(first_user.at("content").at(1).at("cache_control").at("type") == "ephemeral");

  CHECK(messages.at(2).at("tool_calls").at(0).at("function").at("name") == "draw");
  CHECK(messages.at(3).at("content").is_string());
  CHECK(messages.at(3).at("content") == "rendered");
  CHECK(messages.at(4).at("content").at(0).at("text") == "[From the result of call_1:]");
  CHECK(messages.at(4).at("content").at(1).at("image_url").at("url") ==
        "https://example.test/out.png");

  CHECK(body.at("tools").size() == 1);
  CHECK(body.at("business").at("stage") == "processing");
  CHECK(body.at("parameters").at("max_tokens").get<int>() == 1234);
  CHECK(body.at("parameters").at("context_length").get<int>() == 200'000);
}
