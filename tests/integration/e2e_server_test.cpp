// GCC 14 pairs Asio's awaitable-frame `std::aligned_alloc` with `std::free` and then warns about
// the mismatch, which is the allocation pair Asio itself uses and a legal one.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "common/json.hpp"
#include "config/credentials.hpp"
#include "integration/process_runner.hpp"
#include "stub_upstream/stub_logic.hpp"
#include "support/async_test.hpp"
#include "support/server_harness.hpp"
#include "support/temp_dir.hpp"

#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace asio = boost::asio;

using qoderz::json;
using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::test::build_request;
using qoderz::test::header_of;
using qoderz::test::http_result;
using qoderz::test::one_request;
using qoderz::test::process_runner;
using qoderz::test::process_stream;
using qoderz::test::run_awaitable;
using qoderz::testing::temp_dir;

constexpr std::string_view site_id = "qoder-cn";
constexpr auto ready_timeout = 10s;
constexpr auto exit_timeout = 10s;
constexpr std::string_view false_token = "e2e-access-token-not-real";

std::string request_body(std::size_t padding, bool stream) {
  json messages = json::array({json{{"role", "user"}, {"content", "hello stub"}}});
  if (padding > 0) {
    messages.push_back(json{{"role", "user"}, {"content", std::string(padding, 'y')}});
  }
  json body{{"model", "stub-model"}, {"messages", std::move(messages)}};
  if (stream) {
    body["stream"] = true;
  }
  return body.dump();
}

account_record make_account() {
  account_record record;
  record.m_site = std::string{site_id};
  record.m_name = "work";
  record.m_vpc = "";
  record.m_access_token = std::string{false_token};
  record.m_refresh_token = "e2e-refresh-token-not-real";
  // A far-future expiry keeps the happy path from refreshing the token first; the refresh path
  // itself is covered by `tests/unit/qoder/auth`.
  record.m_expires_at = 4102444800000000LL;
  record.m_uid = "uid-e2e";
  record.m_display_name = "E2E";
  record.m_email = "e2e@example.invalid";
  record.m_machine_id = "machine-e2e-not-real";
  return record;
}

std::optional<int> port_of(std::string const& line) {
  auto const marker = line.rfind(':');
  if (marker == std::string::npos) {
    return std::nullopt;
  }
  try {
    return std::stoi(line.substr(marker + 1));
  } catch (std::exception const&) {
    return std::nullopt;
  }
}

/// A real `qoderz` process, a real `qoderz_stub_upstream` process, a temporary credential file and
/// nothing outside `127.0.0.1`. HOME and the XDG directories point into the temporary directory so
/// a bug in the gateway cannot read or write the developer's real files.
struct e2e_fixture {
  temp_dir m_dir;
  credential_store m_store{m_dir.file("credentials.json")};
  std::optional<process_runner> m_upstream;
  std::optional<process_runner> m_gateway;
  std::uint16_t m_upstream_port = 0;
  std::uint16_t m_port = 0;

  explicit e2e_fixture(std::optional<std::string> api_key = {},
                       std::vector<std::string> extra_arguments = {}, bool with_account = true) {
    REQUIRE(start_upstream());
    if (with_account) {
      store({make_account()});
    }
    REQUIRE(start_gateway(api_key, extra_arguments));
  }

  ~e2e_fixture() {
    if (m_gateway.has_value()) {
      (void)m_gateway->terminate(exit_timeout);
    }
    if (m_upstream.has_value()) {
      (void)m_upstream->terminate(exit_timeout);
    }
  }

  e2e_fixture(e2e_fixture const&) = delete;
  e2e_fixture& operator=(e2e_fixture const&) = delete;

  [[nodiscard]] bool start_upstream() {
    auto started = process_runner::start(
        process_runner::options{.m_executable = std::string{QODERZ_STUB_UPSTREAM},
                                .m_arguments = {"--listen", "http://127.0.0.1:0"},
                                .m_environment = {}});
    if (!started.has_value()) {
      return false;
    }
    auto const line =
        started->wait_for_line(process_stream::out, "stub-upstream listening on", ready_timeout);
    if (!line.has_value()) {
      return false;
    }
    auto const parsed = port_of(*line);
    if (!parsed.has_value()) {
      return false;
    }
    m_upstream_port = static_cast<std::uint16_t>(*parsed);
    m_upstream = std::move(*started);
    return true;
  }

  [[nodiscard]] bool start_gateway(std::optional<std::string> api_key,
                                   std::vector<std::string> const& extra_arguments) {
    std::vector<std::string> arguments{"serve",
                                       "--listen",
                                       "http://127.0.0.1:0",
                                       "--site",
                                       std::string{site_id},
                                       "--credentials",
                                       m_dir.file("credentials.json").string()};
    if (api_key.has_value()) {
      arguments.insert(arguments.end(), {"--api-key", *api_key});
    }
    arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());

    auto started = process_runner::start(process_runner::options{
        .m_executable = std::string{QODERZ_BINARY},
        .m_arguments = std::move(arguments),
        .m_environment = {
            {"QODERZ_UPSTREAM", "http://127.0.0.1:" + std::to_string(m_upstream_port)},
            {"HOME", m_dir.path().string()},
            {"XDG_CONFIG_HOME", m_dir.file("xdg").string()},
            {"XDG_DATA_HOME", m_dir.file("data").string()},
            {"QODERZ_API_KEY", ""},
            {"QODERZ_ACCOUNT", ""},
            {"QODERZ_THREADS", "2"},
            {"QODERZ_LOG_LEVEL", "info"}}});
    if (!started.has_value()) {
      return false;
    }
    auto const line = started->wait_for_line(process_stream::err, "listen:", ready_timeout);
    if (!line.has_value()) {
      return false;
    }
    auto const parsed = port_of(*line);
    if (!parsed.has_value()) {
      return false;
    }
    m_port = static_cast<std::uint16_t>(*parsed);
    m_gateway = std::move(*started);
    return true;
  }

  void store(std::vector<account_record> accounts) {
    credential_file file;
    for (auto const& account : accounts) {
      file.m_accounts.emplace(account.key(), account);
    }
    if (!accounts.empty()) {
      file.m_defaults[std::string{site_id}] = accounts.front().key();
    }
    REQUIRE(m_store.save(file).has_value());
  }

  [[nodiscard]] std::optional<http_result>
  request(std::string method, std::string target,
          std::vector<std::pair<std::string, std::string>> headers = {}, std::string body = {}) {
    asio::io_context io;
    auto wire = build_request(method, target, headers, body, false);
    auto nested = run_awaitable(io, one_request(m_port, std::move(wire)));
    if (!nested.has_value()) {
      return std::nullopt;
    }
    return std::move(*nested);
  }

  /// The first successful `/healthz` is also the proof that the signal handler of `serve` is armed:
  /// the io_context only starts running after the handler is registered.
  [[nodiscard]] bool await_health() {
    auto const deadline = std::chrono::steady_clock::now() + ready_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto health = request("GET", "/healthz"); health.has_value()) {
        return health->m_status == 200;
      }
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

  [[nodiscard]] bool ready() { return await_health(); }
};

std::vector<std::string> sse_data_lines(std::string const& body) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < body.size()) {
    auto const end = body.find("\n\n", start);
    auto const stop = end == std::string::npos ? body.size() : end;
    auto const frame = body.substr(start, stop - start);
    if (frame.rfind("data: ", 0) == 0) {
      lines.push_back(frame.substr(6));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 2;
  }
  return lines;
}

/// The inner chunk of one SSE line. The upstream protocol wraps it as a JSON *string* in `body`,
/// so a test that reads the fixture has to unwrap the envelope once.
json inner_of(std::string const& frame) {
  auto const envelope = json::parse(frame, nullptr, false);
  if (!envelope.is_object() || !envelope.contains("body")) {
    return json{};
  }
  return json::parse(envelope.at("body").get<std::string>(), nullptr, false);
}

} // namespace

TEST_CASE("the stub upstream fixture has the documented shape", "[e2e][stub]") {
  auto const listing = json::parse(qoderz::test::stub_model_listing_body(), nullptr, false);
  REQUIRE(listing.is_object());
  REQUIRE(listing.at("app").is_array());
  CHECK(listing.at("app").at(0).at("key") == "stub-model");
  CHECK(listing.at("app").at(0).at("display_name") == "Stub Model");
  CHECK(listing.at("app").at(0).at("enable") == true);

  qoderz::test::stub_upstream_options const options{.m_chunks = 3, .m_chunk_bytes = 4};
  CHECK(qoderz::test::stub_completion_length(options) == 12);
  auto const frames = sse_data_lines(qoderz::test::stub_chat_stream_body(options));
  REQUIRE(frames.size() == 4);
  CHECK(json::parse(frames.back(), nullptr, false).at("body") == "[DONE]");
  auto const last = inner_of(frames.at(2));
  CHECK(last.at("choices").at(0).at("finish_reason") == "stop");
  CHECK(last.at("usage").at("completion_tokens") == 3);
  CHECK(inner_of(frames.front()).at("choices").at(0).at("delta").at("content") == "dddd");

  qoderz::test::stub_request chat;
  chat.m_method = "POST";
  chat.m_target = "/algo/api/v2/service/pro/sse/agent_chat_generation?Encode=1";
  auto const routed = qoderz::test::stub_route(chat, options);
  CHECK(routed.m_handled);
  CHECK(routed.m_response.m_chunked);

  qoderz::test::stub_request unknown;
  unknown.m_method = "GET";
  unknown.m_target = "/nope";
  auto const missing = qoderz::test::stub_route(unknown, options);
  CHECK_FALSE(missing.m_handled);
  CHECK(missing.m_response.m_status == 200);
  CHECK(missing.m_response.m_body == "{}");
}

TEST_CASE("a real gateway answers health and models against a stub upstream", "[e2e]") {
  e2e_fixture fixture;
  REQUIRE(fixture.ready());

  auto const health = fixture.request("GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);
  CHECK(json::parse(health->m_body, nullptr, false).at("status") == "ok");

  auto const models = fixture.request("GET", "/v1/models");
  REQUIRE(models.has_value());
  CHECK(models->m_status == 200);
  auto const listing = json::parse(models->m_body, nullptr, false);
  REQUIRE(listing.is_object());
  CHECK(listing.at("object") == "list");
  REQUIRE(listing.at("data").is_array());
  CHECK(listing.at("data").at(0).at("id") == "stub-model");
  CHECK(listing.at("data").at(0).at("context_window") == 1000000);
  CHECK(models->m_body.find("e2e-access-token-not-real") == std::string::npos);
}

TEST_CASE("a non streaming chat copies the stub completion and its usage", "[e2e]") {
  e2e_fixture fixture;
  REQUIRE(fixture.ready());

  auto const chat = fixture.request("POST", "/v1/chat/completions", {}, request_body(0, false));
  REQUIRE(chat.has_value());
  CHECK(chat->m_status == 200);
  auto const answer = json::parse(chat->m_body, nullptr, false);
  REQUIRE(answer.is_object());
  CHECK(answer.at("object") == "chat.completion");
  REQUIRE(answer.at("choices").is_array());
  CHECK(answer.at("choices").at(0).at("finish_reason") == "stop");
  auto const content = answer.at("choices").at(0).at("message").at("content").get<std::string>();
  CHECK(content == std::string(8 * 256, 'd'));
  REQUIRE(answer.contains("usage"));
  CHECK(answer.at("usage").at("prompt_tokens") == 1000);
  CHECK(answer.at("usage").at("completion_tokens") == 8);
  CHECK(answer.at("usage").at("total_tokens") == 1008);
}

TEST_CASE("a streaming chat forwards role, content, stop and DONE frames", "[e2e]") {
  e2e_fixture fixture;
  REQUIRE(fixture.ready());

  auto const chat = fixture.request("POST", "/v1/chat/completions", {}, request_body(0, true));
  REQUIRE(chat.has_value());
  CHECK(chat->m_status == 200);
  CHECK(header_of(*chat, "Content-Type") == "text/event-stream");

  auto const frames = sse_data_lines(chat->m_body);
  REQUIRE(frames.size() >= 3);
  CHECK(frames.back() == "[DONE]");

  auto const first = json::parse(frames.front(), nullptr, false);
  REQUIRE(first.is_object());
  CHECK(first.at("object") == "chat.completion.chunk");
  CHECK(first.at("choices").at(0).at("delta").at("role") == "assistant");

  std::string text;
  bool saw_stop = false;
  std::int64_t completion_tokens = 0;
  for (std::size_t index = 1; index + 1 < frames.size(); ++index) {
    auto const frame = json::parse(frames[index], nullptr, false);
    REQUIRE(frame.is_object());
    if (!frame.contains("choices") || !frame.at("choices").is_array() ||
        frame.at("choices").empty()) {
      continue;
    }
    auto const& choice = frame.at("choices").at(0);
    if (choice.contains("delta") && choice.at("delta").contains("content")) {
      text.append(choice.at("delta").at("content").get<std::string>());
    }
    if (choice.contains("finish_reason") && !choice.at("finish_reason").is_null()) {
      CHECK(choice.at("finish_reason") == "stop");
      saw_stop = true;
      if (frame.contains("usage")) {
        completion_tokens = frame.at("usage").at("completion_tokens").get<std::int64_t>();
      }
    }
  }
  CHECK(text == std::string(8 * 256, 'd'));
  CHECK(saw_stop);
  CHECK(completion_tokens == 8);
}

TEST_CASE("the API key guards /v1 but never /healthz", "[e2e]") {
  SECTION("without a configured key every request is served") {
    e2e_fixture fixture;
    REQUIRE(fixture.ready());
    auto const models = fixture.request("GET", "/v1/models");
    REQUIRE(models.has_value());
    CHECK(models->m_status == 200);
  }

  SECTION("a configured key is required for /v1") {
    e2e_fixture fixture{std::string{"e2e-api-key-not-real"}};
    REQUIRE(fixture.ready());

    auto const missing = fixture.request("GET", "/v1/models");
    REQUIRE(missing.has_value());
    CHECK(missing->m_status == 401);

    auto const wrong = fixture.request("GET", "/v1/models", {{"Authorization", "Bearer nope"}});
    REQUIRE(wrong.has_value());
    CHECK(wrong->m_status == 401);

    auto const good =
        fixture.request("GET", "/v1/models", {{"Authorization", "Bearer e2e-api-key-not-real"}});
    REQUIRE(good.has_value());
    CHECK(good->m_status == 200);

    auto const health = fixture.request("GET", "/healthz");
    REQUIRE(health.has_value());
    CHECK(health->m_status == 200);

    CHECK(fixture.m_gateway->captured(process_stream::err).find("e2e-api-key-not-real") ==
          std::string::npos);
  }
}

TEST_CASE("unknown paths, wrong methods and oversized bodies keep their statuses", "[e2e]") {
  e2e_fixture fixture{std::nullopt, {"--max-body-bytes", "2048"}};
  REQUIRE(fixture.ready());

  auto const missing = fixture.request("GET", "/v1/nope");
  REQUIRE(missing.has_value());
  CHECK(missing->m_status == 404);
  CHECK(json::parse(missing->m_body, nullptr, false).at("error").at("type") ==
        "invalid_request_error");

  auto const wrong = fixture.request("GET", "/v1/chat/completions");
  REQUIRE(wrong.has_value());
  CHECK(wrong->m_status == 405);
  CHECK(header_of(*wrong, "Allow") == "POST");

  auto const too_large =
      fixture.request("POST", "/v1/chat/completions", {}, request_body(4096, false));
  REQUIRE(too_large.has_value());
  CHECK(too_large->m_status == 413);
  CHECK(json::parse(too_large->m_body, nullptr, false).at("error").at("code") == 413);
}

TEST_CASE("the gateway refuses to start without an account", "[e2e]") {
  temp_dir dir;
  credential_store store{dir.file("credentials.json")};
  REQUIRE(store.save(credential_file{}).has_value());

  auto started = process_runner::start(process_runner::options{
      .m_executable = std::string{QODERZ_BINARY},
      .m_arguments = {"serve", "--listen", "http://127.0.0.1:0", "--site", std::string{site_id},
                      "--credentials", dir.file("credentials.json").string()},
      .m_environment = {{"HOME", dir.path().string()},
                        {"XDG_CONFIG_HOME", dir.file("xdg").string()}}});
  REQUIRE(started.has_value());

  auto const code = started->wait_for_exit(exit_timeout);
  REQUIRE(code.has_value());
  CHECK(*code == 1);
  CHECK(started->captured(process_stream::err).find("qoderz login") != std::string::npos);
}

TEST_CASE("SIGTERM stops a running gateway within the grace period", "[e2e]") {
  e2e_fixture fixture;
  REQUIRE(fixture.ready());

  auto const code = fixture.m_gateway->terminate(exit_timeout);
  REQUIRE(code.has_value());
  CHECK(*code == 0);
  CHECK(fixture.m_gateway->wait_for_exit(1s).has_value());
}
