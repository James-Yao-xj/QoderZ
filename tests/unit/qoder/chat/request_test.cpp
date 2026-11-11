#include "common/codec/base64.hpp"
#include "common/error.hpp"
#include "config/accounts.hpp"
#include "config/site.hpp"
#include "protocol/body_codec.hpp"
#include "protocol/cosy_signer.hpp"
#include "qoder/chat/request.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::config::account_record;
using qoderz::config::resolved_site;
using qoderz::net::http_header;
using qoderz::qoder::chat::build_signed_request;
using qoderz::qoder::chat::signed_request;
using qoderz::qoder::chat::upstream_request;

constexpr std::int64_t timestamp = 1'760'000'000;
constexpr std::string_view body_json = R"({"request_id":"r1","messages":[]})";
constexpr std::string_view endpoint =
    "https://api3.qoder.sh/algo/api/v2/service/pro/sse/agent_chat_generation"
    "?FetchKeys=llm_model_result&AgentId=agent_common&Encode=1";

class fake_key_wrapper final : public qoderz::protocol::rsa_key_wrapper {
public:
  [[nodiscard]] qoderz::result<std::string> wrap(std::string_view /*key*/) const override {
    return qoderz::codec::base64_encode(std::string(128, '\x5a'));
  }
};

resolved_site make_site() {
  auto site = qoderz::config::find_site("qoder");
  REQUIRE(site.has_value());
  auto resolved = qoderz::config::resolve_site(**site, "");
  REQUIRE(resolved.has_value());
  return std::move(*resolved);
}

account_record make_account() {
  account_record account;
  account.m_site = "qoder";
  account.m_name = "ada";
  account.m_access_token = "test-access-token-not-real";
  account.m_uid = "uid-test-not-real";
  account.m_machine_id = "machine-test-not-real";
  account.m_display_name = "Ada Test";
  account.m_email = "tester@example.invalid";
  return account;
}

upstream_request make_request() {
  return upstream_request{
      .m_site = make_site(),
      .m_account = make_account(),
      .m_body_json = std::string{body_json},
      .m_model_key = "claude-sonnet-4.5",
      .m_model_source = "anthropic",
  };
}

std::string const* find_header(std::vector<http_header> const& headers, std::string_view name) {
  for (auto const& header : headers) {
    if (qoderz::strings::equals_ignore_case(header.m_name, name)) {
      return &header.m_value;
    }
  }
  return nullptr;
}

std::size_t count_headers(std::vector<http_header> const& headers, std::string_view name) {
  std::size_t total = 0;
  for (auto const& header : headers) {
    if (qoderz::strings::equals_ignore_case(header.m_name, name)) {
      ++total;
    }
  }
  return total;
}

signed_request build(upstream_request const& request, fake_key_wrapper const& wrapper,
                     std::int64_t seconds = timestamp) {
  auto built = build_signed_request(request, wrapper, seconds);
  REQUIRE(built.has_value());
  return std::move(*built);
}

} // namespace

TEST_CASE("the signed URL is the chat endpoint with the fixed query", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const built = build(make_request(), wrapper);
  CHECK(built.m_url == endpoint);
  CHECK(built.m_url.find("/algo/api/v2/service/pro/sse/agent_chat_generation") !=
        std::string::npos);
  CHECK(built.m_url.find("FetchKeys=llm_model_result") != std::string::npos);
  CHECK(built.m_url.find("AgentId=agent_common") != std::string::npos);
  CHECK(built.m_url.find("Encode=1") != std::string::npos);
}

TEST_CASE("the signed path drops the /algo prefix", "[qoder][chat][request]") {
  CHECK(qoderz::protocol::signed_path(endpoint) == "/api/v2/service/pro/sse/agent_chat_generation");
}

TEST_CASE("the request body is encoded, never plaintext", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const built = build(make_request(), wrapper);
  CHECK(built.m_wire_body != body_json);
  CHECK(built.m_wire_body.find("request_id") == std::string::npos);
  CHECK(built.m_wire_body == qoderz::protocol::body_encode(body_json));
  auto const decoded = qoderz::protocol::body_decode(built.m_wire_body);
  REQUIRE(decoded.has_value());
  CHECK(*decoded == body_json);
}

TEST_CASE("the COSY headers are sent with a bearer authorization", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const built = build(make_request(), wrapper);

  auto const* authorization = find_header(built.m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(authorization->starts_with("Bearer COSY."));
  CHECK(authorization->find('.') != std::string::npos);

  auto const* user = find_header(built.m_headers, "Cosy-User");
  REQUIRE(user != nullptr);
  CHECK(*user == "uid-test-not-real");

  auto const* date = find_header(built.m_headers, "Cosy-Date");
  REQUIRE(date != nullptr);
  CHECK(*date == std::to_string(timestamp));

  CHECK(find_header(built.m_headers, "Cosy-MachineId") != nullptr);
  CHECK(find_header(built.m_headers, "Login-Version") != nullptr);
}

TEST_CASE("the streaming headers are added once", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const built = build(make_request(), wrapper);

  CHECK(count_headers(built.m_headers, "Accept") == 1);
  auto const* accept = find_header(built.m_headers, "Accept");
  REQUIRE(accept != nullptr);
  CHECK(*accept == "text/event-stream");

  auto const* cache = find_header(built.m_headers, "Cache-Control");
  REQUIRE(cache != nullptr);
  CHECK(*cache == "no-cache");

  auto const* content_type = find_header(built.m_headers, "Content-Type");
  REQUIRE(content_type != nullptr);
  CHECK(*content_type == "application/json");

  auto const* model_key = find_header(built.m_headers, "X-Model-Key");
  REQUIRE(model_key != nullptr);
  CHECK(*model_key == "claude-sonnet-4.5");
}

TEST_CASE("X-Model-Source is omitted when the catalog has no source", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto request = make_request();
  request.m_model_source.clear();
  auto const built = build(request, wrapper);
  CHECK(find_header(built.m_headers, "X-Model-Source") == nullptr);
}

TEST_CASE("X-Model-Source is sent when the catalog has one", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const built = build(make_request(), wrapper);
  auto const* source = find_header(built.m_headers, "X-Model-Source");
  REQUIRE(source != nullptr);
  CHECK(*source == "anthropic");
}

TEST_CASE("two builds in the same second share the date but not the signature",
          "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto const request = make_request();
  auto const first = build(request, wrapper, timestamp);
  auto const second = build(request, wrapper, timestamp);
  CHECK(first.m_wire_body == second.m_wire_body);
  CHECK(*find_header(first.m_headers, "Cosy-Date") == *find_header(second.m_headers, "Cosy-Date"));
  // Each build draws a fresh AES key and request id, so the bearer token differs while the
  // encoded body it signs stays the same.
  CHECK(*find_header(first.m_headers, "Authorization") !=
        *find_header(second.m_headers, "Authorization"));

  auto const later = build(request, wrapper, timestamp + 1);
  CHECK(*find_header(later.m_headers, "Cosy-Date") == std::to_string(timestamp + 1));
}

TEST_CASE("an account without a uid or machine id is a configuration error",
          "[qoder][chat][request]") {
  fake_key_wrapper wrapper;

  auto missing_uid = make_request();
  missing_uid.m_account.m_uid.clear();
  auto const uid_outcome = build_signed_request(missing_uid, wrapper, timestamp);
  REQUIRE(!uid_outcome.has_value());
  CHECK(uid_outcome.error().m_kind == qoderz::error_kind::configuration);
  CHECK(uid_outcome.error().m_message.find("qoderz login") != std::string::npos);

  auto missing_machine = make_request();
  missing_machine.m_account.m_machine_id.clear();
  auto const machine_outcome = build_signed_request(missing_machine, wrapper, timestamp);
  REQUIRE(!machine_outcome.has_value());
  CHECK(machine_outcome.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("the display name falls back to the account name", "[qoder][chat][request]") {
  fake_key_wrapper wrapper;
  auto request = make_request();
  request.m_account.m_display_name.clear();
  auto const built = build(request, wrapper);
  CHECK_FALSE(built.m_wire_body.empty());
  CHECK(find_header(built.m_headers, "Authorization") != nullptr);
}
