#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/body.hpp"
#include "openai/request/request.hpp"
#include "openai/request/types.hpp"
#include "openai/selection/session.hpp"
#include "qoder/catalog/model.hpp"
#include "support/fake_clock.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using qoderz::clock;
using qoderz::error_kind;
using qoderz::json;
using qoderz::openai::prepare_request;
using qoderz::openai::request_context;
using qoderz::openai::selection::session_store;
using qoderz::qoder::catalog::model_info;
using qoderz::test::fake_clock;

constexpr std::string_view system_prefix =
    "You are a Qoder agent. Use the instructions below and the tools available to you to assist "
    "the user.";
constexpr std::string_view required_line = "You must call an available function in this response.";
constexpr std::string_view empty_schema = R"({"type":"object","properties":{}})";

model_info fixture_model() {
  model_info model;
  model.m_key = "performance";
  model.m_name = "Performance";
  model.m_context = 1'000'000;
  model.m_default_window = 200'000;
  model.m_windows = {200'000, 1'000'000};
  model.m_thinks = true;
  model.m_efforts = {"low", "medium", "high"};
  model.m_default_effort = "medium";
  model.m_raw_json = R"({"key":"performance"})";
  return model;
}

request_context prepare(json const& body, std::optional<std::string_view> header,
                        session_store& store, clock& time) {
  auto prepared_body = prepare_request(body, header, fixture_model(), store, time);
  REQUIRE(prepared_body.has_value());
  return *prepared_body;
}

} // namespace

TEST_CASE("consuming a prepared request preserves translation and retry metadata",
          "[openai][request][memory]") {
  session_store store;
  fake_clock time;
  auto body = json::parse(R"({"messages":[{"role":"system","content":"instructions"},
    {"role":"user","content":"placeholder"}],"reasoning_effort":"high",
    "tools":[{"type":"function","function":{"name":"lookup","parameters":{"type":"object"}}}]})");
  body["messages"][1]["content"] = std::string(1024 * 1024, 'x');
  auto prepared = qoderz::openai::read_request(body, "retry-session", store);
  REQUIRE(prepared.has_value());
  auto copied = prepare_request(*prepared, fixture_model(), store, time);
  REQUIRE(copied.has_value());
  auto const* message_storage = prepared->m_dialog.m_messages.data();
  auto const* tool_storage = prepared->m_tools.data();
  auto moved = prepare_request(std::move(*prepared), fixture_model(), store, time);
  REQUIRE(moved.has_value());
  CHECK(moved->m_messages.data() == message_storage);
  CHECK(moved->m_tools.data() == tool_storage);
  CHECK(prepared->m_dialog.m_messages.empty());
  CHECK(prepared->m_tools.empty());
  CHECK(prepared->m_session_id == copied->m_session_id);
  CHECK(prepared->m_dialog.m_last_user_text.size() == 1024 * 1024);
  CHECK(prepared->m_dialog.m_first_user_text.empty());
  CHECK(moved->m_task.m_id == copied->m_task.m_id);
  moved->m_request_id = copied->m_request_id;
  auto const expected = qoderz::openai::build_qoder_body(*copied);
  auto const actual = qoderz::openai::build_qoder_body(*moved);
  REQUIRE(expected.has_value());
  REQUIRE(actual.has_value());
  CHECK(actual->m_json == expected->m_json);
}

TEST_CASE("A request without messages is a bad request", "[openai][request]") {
  session_store store;
  fake_clock time;

  for (auto const* body : {R"({})", R"({"messages":"hi"})", R"({"messages":{"role":"user"}})",
                           R"({"messages":null})"}) {
    auto const prepared_body =
        prepare_request(json::parse(body), std::nullopt, fixture_model(), store, time);
    INFO(body);
    REQUIRE_FALSE(prepared_body.has_value());
    CHECK(prepared_body.error().m_kind == error_kind::bad_request);
    CHECK(prepared_body.error().m_message == "messages is required");
    CHECK(prepared_body.error().m_http_status == 400);
  }
}

TEST_CASE("An empty message array is a valid request", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[]})"), std::nullopt, store, time);

  CHECK(context.m_messages.empty());
  CHECK(context.m_system_text == system_prefix);
  CHECK(context.m_max_tokens == 32000);
}

TEST_CASE("The system text starts with the fixed prefix", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"Hi"}]})"),
                               std::nullopt, store, time);

  CHECK(context.m_system_text == system_prefix);
  CHECK(context.m_messages.size() == 1);
}

TEST_CASE("System and developer entries are merged in order", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[
        {"role":"system","content":"First"},
        {"role":"developer","content":[{"type":"text","text":"Second"}]},
        {"role":"user","content":"Hi"}
      ]})"),
                               std::nullopt, store, time);

  auto const expected = std::string{system_prefix} + "\n\nFirst\n\nSecond";
  CHECK(context.m_system_text == expected);
  // The merged entries must not reappear as their own messages.
  REQUIRE(context.m_messages.size() == 1);
  CHECK(context.m_messages[0].m_role == "user");
}

TEST_CASE("System text keeps whitespace and skips empty entries", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[
        {"role":"system","content":"  padded  "},
        {"role":"system","content":""},
        {"role":"system","content":[{"type":"text","text":""}]},
        {"role":"user","content":"Hi"}
      ]})"),
                               std::nullopt, store, time);

  CHECK(context.m_system_text == std::string{system_prefix} + "\n\n  padded  ");
}

TEST_CASE("Roles are matched exactly and case-sensitively", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[
        {"role":" system ","content":"spaced"},
        {"role":"System","content":"wrong case"},
        {"role":"user","content":"Hi"}
      ]})"),
                               std::nullopt, store, time);

  CHECK(context.m_system_text == std::string{system_prefix} + "\n\nspaced");
}

TEST_CASE("A required tool choice appends the instruction", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"weather"}],
        "tools":[{"type":"function","function":{"name":"get_weather"}}],
        "tool_choice":" REQUIRED "})"),
                               std::nullopt, store, time);

  CHECK(context.m_include_tools);
  CHECK(context.m_system_text == std::string{system_prefix} + "\n" + std::string{required_line});
}

TEST_CASE("Tool choice none drops the tools and the instruction", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"weather"}],
        "tools":[{"type":"function","function":{"name":"get_weather"}}],
        "tool_choice":"none"})"),
                               std::nullopt, store, time);

  CHECK_FALSE(context.m_include_tools);
  // The schemas are still parsed; `include_tools` alone decides whether they get written.
  CHECK(context.m_tools.size() == 1);
  CHECK(context.m_system_text == system_prefix);
}

TEST_CASE("Tools are kept for auto, an object and any other choice", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const body_text = std::string{R"({"messages":[{"role":"user","content":"x"}],
      "tools":[{"type":"function","function":{"name":"get_weather"}}],
      "tool_choice":)"};
  for (auto const* choice : {R"("auto")", R"({"type":"function","function":{"name":"x"}})",
                             R"("whatever")", R"(42)", "null"}) {
    auto const context = prepare(json::parse(body_text + choice + "}"), std::nullopt, store, time);
    INFO(choice);
    CHECK(context.m_include_tools);
    CHECK(context.m_tools.size() == 1);
  }
}

TEST_CASE("An unusable tool entry is skipped", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"tools":[
        {"type":"function","function":{"name":"  "}},
        {"type":"function","function":"not-an-object"},
        {"type":"function"},
        "not-an-entry",
        {"type":"function","function":{"name":" usable "}}
      ]})"),
                               std::nullopt, store, time);

  REQUIRE(context.m_tools.size() == 1);
  CHECK(context.m_tools[0].m_name == "usable");
}

TEST_CASE("A tool without description or parameters gets the defaults", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"tools":[
        {"type":"function","function":{"name":"bare"}},
        {"type":"function","function":{"name":"bad","parameters":"nope"}},
        {"type":"function","function":{"name":"ok","parameters":{"type":"object",
          "properties":{"city":{"type":"string"}}}}}
      ]})"),
                               std::nullopt, store, time);

  REQUIRE(context.m_tools.size() == 3);
  CHECK(context.m_tools[0].m_description.empty());
  CHECK(context.m_tools[0].m_parameters_json == empty_schema);
  CHECK(context.m_tools[1].m_parameters_json == empty_schema);
  CHECK(context.m_tools[2].m_parameters_json ==
        R"({"properties":{"city":{"type":"string"}},"type":"object"})");
}

TEST_CASE("A boolean parameters value is preserved as written", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"tools":[
        {"type":"function","function":{"name":"yes","parameters":true}},
        {"type":"function","function":{"name":"no","parameters":false}}
      ]})"),
                               std::nullopt, store, time);

  REQUIRE(context.m_tools.size() == 2);
  CHECK(context.m_tools[0].m_parameters_json == "true");
  CHECK(context.m_tools[1].m_parameters_json == "false");
}

TEST_CASE("A non-array tools field means no tools", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context =
      prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"tools":{"name":"a"}})"),
              std::nullopt, store, time);

  CHECK_FALSE(context.m_include_tools);
  CHECK(context.m_tools.empty());
}

TEST_CASE("The token limit prefers max_completion_tokens", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],
        "max_completion_tokens":1234,"max_tokens":999})"),
                               std::nullopt, store, time);

  CHECK(context.m_max_tokens == 1234);
}

TEST_CASE("An unusable token limit falls back to the default", "[openai][request]") {
  session_store store;
  fake_clock time;

  for (auto const* fields : {R"("max_completion_tokens":0,"max_tokens":0)",
                             R"("max_completion_tokens":-5)", R"("max_completion_tokens":"1234")",
                             R"("max_completion_tokens":1.5)", R"("max_completion_tokens":1e30)"}) {
    auto const context = prepare(
        json::parse(R"({"messages":[{"role":"user","content":"x"}],)" + std::string{fields} + "}"),
        std::nullopt, store, time);
    INFO(fields);
    CHECK(context.m_max_tokens == 32000);
  }
}

TEST_CASE("A float that is a whole number is a usable token limit", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context =
      prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"max_tokens":2048.0})"),
              std::nullopt, store, time);

  CHECK(context.m_max_tokens == 2048);
}

TEST_CASE("The second token field is used when the first is unusable", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],
        "max_completion_tokens":0,"max_tokens":777})"),
                               std::nullopt, store, time);

  CHECK(context.m_max_tokens == 777);
}

TEST_CASE("The requested effort is trimmed before selection", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(
      json::parse(R"({"messages":[{"role":"user","content":"x"}],"reasoning_effort":" HIGH "})"),
      std::nullopt, store, time);

  CHECK(context.m_enable_thinking);
  CHECK(context.m_reasoning_effort == "high");
}

TEST_CASE("An absent effort uses the model default", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}]})"),
                               std::nullopt, store, time);

  CHECK(context.m_reasoning_effort == "medium");
}

TEST_CASE("The session id prefers the header and then the request fields", "[openai][request]") {
  session_store store;
  fake_clock time;
  auto const body = json::parse(
      R"({"messages":[{"role":"user","content":"x"}],
          "prompt_cache_key":" key ","user":" user "})");

  auto const from_header = prepare(body, "header-1", store, time);
  auto const from_key = prepare(body, std::nullopt, store, time);
  auto const from_user =
      prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}],"user":" user "})"),
              std::nullopt, store, time);
  auto const from_prompt = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}]})"),
                                   std::nullopt, store, time);

  CHECK(from_header.m_session_id != from_key.m_session_id);
  CHECK(from_key.m_session_id != from_user.m_session_id);
  CHECK(from_user.m_session_id != from_prompt.m_session_id);
  CHECK(from_key.m_session_id ==
        store.make_session_id(std::nullopt, " key ", std::nullopt, from_key.m_system_text, "x"));
}

TEST_CASE("A blank header falls through to the cache key", "[openai][request]") {
  session_store store;
  fake_clock time;
  auto const body =
      json::parse(R"({"messages":[{"role":"user","content":"x"}],"prompt_cache_key":"k1"})");

  CHECK(prepare(body, "   ", store, time).m_session_id ==
        prepare(body, std::nullopt, store, time).m_session_id);
}

TEST_CASE("The same turn reuses its task and a new request id is minted", "[openai][request]") {
  session_store store;
  fake_clock time;
  auto const body = json::parse(R"({"messages":[{"role":"user","content":"hello"}]})");

  auto const first = prepare(body, std::nullopt, store, time);
  auto const second = prepare(body, std::nullopt, store, time);

  CHECK(first.m_task.m_id == second.m_task.m_id);
  CHECK(first.m_task.m_begin_at_ms == second.m_task.m_begin_at_ms);
  CHECK_FALSE(first.m_task.m_answered);
  CHECK(first.m_request_id != second.m_request_id);
  CHECK(first.m_task.m_name == "hello");
}

TEST_CASE("A different prompt starts a different task", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const first = prepare(json::parse(R"({"messages":[{"role":"user","content":"hello"}]})"),
                             std::nullopt, store, time);
  auto const other = prepare(json::parse(R"({"messages":[{"role":"user","content":"goodbye"}]})"),
                             std::nullopt, store, time);

  CHECK(first.m_task.m_id != other.m_task.m_id);
}

TEST_CASE("The task records the injected clock", "[openai][request]") {
  session_store store;
  fake_clock time{1'760'000'000'123};

  auto const context = prepare(json::parse(R"({"messages":[{"role":"user","content":"x"}]})"),
                               std::nullopt, store, time);

  CHECK(context.m_task.m_begin_at_ms == 1'760'000'000'123);
}

TEST_CASE("Messages reach the context normalised", "[openai][request]") {
  session_store store;
  fake_clock time;

  auto const context = prepare(json::parse(R"({"messages":[
        {"role":"user","content":"weather?"},
        {"role":"assistant","content":"checking","tool_calls":[
          {"id":"call_1","type":"function","function":{"name":"get_weather","arguments":"{}"}}
        ]},
        {"role":"tool","tool_call_id":"call_1","content":"sunny"}
      ]})"),
                               std::nullopt, store, time);

  REQUIRE(context.m_messages.size() == 3);
  CHECK(context.m_messages[0].m_role == "user");
  CHECK(context.m_messages[1].m_role == "assistant");
  CHECK(context.m_messages[2].m_role == "tool");
  CHECK(context.m_messages[2].m_tool_call_id == "call_1");
}

TEST_CASE("A non-object body is a bad request", "[openai][request]") {
  session_store store;
  fake_clock time;

  for (auto const* body : {R"([])", R"("text")", "42", "null"}) {
    auto const prepared_body =
        prepare_request(json::parse(body), std::nullopt, fixture_model(), store, time);
    INFO(body);
    REQUIRE_FALSE(prepared_body.has_value());
    CHECK(prepared_body.error().m_kind == error_kind::bad_request);
  }
}
