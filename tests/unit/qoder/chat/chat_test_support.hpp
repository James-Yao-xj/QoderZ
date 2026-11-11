#pragma once

#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/accounts.hpp"
#include "config/site.hpp"
#include "protocol/cosy_signer.hpp"
#include "protocol/sse/chat_event.hpp"
#include "qoder/chat/client.hpp"
#include "qoder/chat/stream.hpp"
#include "qoder/chat/types.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Fixtures shared by the tests of `chat_client`, kept out of the cases so each file reads as the
/// behaviour it checks.
namespace qoderz::test::chat {

namespace asio = boost::asio;

using qoderz::error_kind;
using qoderz::config::account_record;
using qoderz::config::resolved_site;
using qoderz::protocol::chat_event;
using qoderz::protocol::chat_event_kind;
using qoderz::qoder::chat::chat_client;
using qoderz::qoder::chat::retry_options;
using qoderz::qoder::chat::upstream_request;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using namespace std::chrono_literals;

constexpr std::string_view chat_target = "/algo/api/v2/service/pro/sse/agent_chat_generation";

/// A wrapper with a fixed ciphertext: RSA wrapping is pinned by the cosy tests, not here.
struct passthrough_wrapper final : qoderz::protocol::rsa_key_wrapper {
  [[nodiscard]] qoderz::result<std::string> wrap(std::string_view) const override {
    return std::string(128, '\x5a');
  }
};

/// One `data:` value carrying `body` as the envelope's JSON string.
inline std::string envelope(std::string_view body) {
  std::string line = R"({"statusCodeValue":200,"body":")";
  for (char const character : body) {
    if (character == '"' || character == '\\') {
      line.push_back('\\');
    }
    line.push_back(character);
  }
  line.append("\"}");
  return line;
}

inline std::string frame(std::string_view body) {
  return "data: " + envelope(body) + "\n";
}

inline constexpr std::string_view hello =
    R"({"choices":[{"index":0,"delta":{"content":"Hello"}}]})";
inline constexpr std::string_view world =
    R"({"choices":[{"index":0,"delta":{"content":" world"},"finish_reason":"stop"}]})";

inline std::string success_stream() {
  std::string body;
  body.append(frame(hello));
  body.append(frame(world));
  body.append("data: ").append(envelope("[DONE]")).append("\n");
  return body;
}

inline constexpr std::string_view queue_body =
    R"({"code":"10605","queue":{"isQueued":true,"retryAfterSeconds":1},"message":"queued"})";

/// A failure envelope carrying the queue body, which is what a queued response actually looks like.
inline std::string queue_frame() {
  std::string line = R"({"statusCodeValue":503,"body":")";
  for (char const character : queue_body) {
    if (character == '"' || character == '\\') {
      line.push_back('\\');
    }
    line.push_back(character);
  }
  line.append("\"}");
  return line;
}

/// The public site table entry, before any loopback override.
inline resolved_site make_site_without_server() {
  auto site = qoderz::config::find_site("qoder");
  REQUIRE(site.has_value());
  auto resolved = qoderz::config::resolve_site(**site, "");
  REQUIRE(resolved.has_value());
  return std::move(*resolved);
}

/// The loopback site, so no name is ever resolved and no real host is contacted.
inline resolved_site make_site(stub_server& server) {
  auto overridden =
      qoderz::config::apply_loopback_override(make_site_without_server(), server.origin());
  REQUIRE(overridden.has_value());
  return std::move(*overridden);
}

inline account_record make_account() {
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

inline upstream_request make_request(stub_server& server) {
  return upstream_request{
      .m_site = make_site(server),
      .m_account = make_account(),
      .m_body_json = R"({"request_id":"r1"})",
      .m_model_key = "claude-sonnet-4.5",
      .m_model_source = "",
  };
}

/// A stub that answers in the order the test queued its responses and remembers what it saw.
struct scripted_stub {
  std::vector<stub_response> m_answers;
  std::vector<stub_request> m_requests;
  std::size_t m_answer_index = 0;

  stub_server make(asio::io_context& io) {
    return stub_server{io, [this](stub_request const& request) -> asio::awaitable<stub_response> {
                         m_requests.push_back(request);
                         auto const index = m_answer_index < m_answers.size()
                                                ? m_answer_index
                                                : m_answers.size() - 1;
                         ++m_answer_index;
                         co_return m_answers[index];
                       }};
  }
};

inline stub_response text_response(int status, std::string body) {
  stub_response response;
  response.m_status = status;
  response.m_body = std::move(body);
  response.m_headers.push_back(
      qoderz::net::http_header{.m_name = "Content-Type", .m_value = "text/event-stream"});
  return response;
}

/// Everything a drained stream produced, so a case asserts on the whole sequence at once.
struct drained {
  std::vector<chat_event> m_events;
};

inline asio::awaitable<qoderz::result<drained>>
drain(std::unique_ptr<qoderz::qoder::chat::chat_stream> stream) {
  drained collected;
  for (;;) {
    auto event = co_await stream->next();
    if (!event) {
      co_return std::unexpected(event.error());
    }
    if (!event->has_value()) {
      co_return collected;
    }
    collected.m_events.push_back(std::move(**event));
  }
}

/// Opens the request and drains whatever stream comes back, which is what every case needs.
inline asio::awaitable<qoderz::result<drained>> open_and_drain(chat_client& client,
                                                               upstream_request const& request) {
  auto stream = co_await client.open(request);
  if (!stream) {
    co_return std::unexpected(stream.error());
  }
  co_return co_await drain(std::move(*stream));
}

inline std::vector<chat_event_kind> kinds_of(std::vector<chat_event> const& events) {
  std::vector<chat_event_kind> kinds;
  kinds.reserve(events.size());
  for (auto const& event : events) {
    kinds.push_back(event.m_kind);
  }
  return kinds;
}

inline std::string const* find_header(std::vector<qoderz::net::http_header> const& headers,
                                      std::string_view name) {
  return qoderz::net::header_value(headers, name);
}

} // namespace qoderz::test::chat
