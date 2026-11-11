#include "common/net/http_client.hpp"
#include "qoder/chat/client.hpp"
#include "support/async_test.hpp"
#include "unit/qoder/chat/chat_test_support.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace qoderz::test::chat;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_server;

} // namespace

TEST_CASE("open returns a stream over a successful response", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  CHECK(kinds_of(events) == std::vector<chat_event_kind>{chat_event_kind::text,
                                                         chat_event_kind::text,
                                                         chat_event_kind::stop});
  CHECK(events[0].m_text == "Hello");
  CHECK(events[1].m_text == " world");
  CHECK(events[2].m_reason == "stop");
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("the request is a signed POST to the chat endpoint", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE(stub.m_requests.size() == 1);
  auto const& request = stub.m_requests.front();
  CHECK(request.m_method == "POST");
  CHECK(request.m_target.starts_with(chat_target));
  CHECK(request.m_target.find("Encode=1") != std::string::npos);
  CHECK(request.m_body.find("request_id") == std::string::npos);

  auto const* authorization = find_header(request.m_headers, "Authorization");
  REQUIRE(authorization != nullptr);
  CHECK(authorization->starts_with("Bearer COSY."));
  auto const* accept = find_header(request.m_headers, "Accept");
  REQUIRE(accept != nullptr);
  CHECK(*accept == "text/event-stream");
  CHECK(find_header(request.m_headers, "X-Model-Key") != nullptr);
}

TEST_CASE("a 401 comes back as auth without a retry", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(401, R"({"code":"105","message":"token expired"})"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, client.open(make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_http_status == 401);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a 429 comes back as quota without a retry", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(429, R"({"code":"110","message":"out of quota"})"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, client.open(make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::quota);
  CHECK(outcome->error().m_http_status == 429);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a 500 comes back as upstream without a retry", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(500, R"({"message":"boom"})"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, client.open(make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_http_status == 500);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a 429 after the first event surfaces as an error event without a stop",
          "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  std::string body;
  body.append(frame(hello));
  body.append(R"(data: {"statusCodeValue":429,"body":{"code":"110","message":"out of quota"}})")
      .append("\n");
  stub.m_answers.push_back(text_response(200, std::move(body)));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(kinds_of(outcome->value().m_events) ==
        std::vector<chat_event_kind>{chat_event_kind::text, chat_event_kind::error});
  CHECK(outcome->value().m_events.back().m_status == 429);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a truncated stream still ends with a stop", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, frame(hello)));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  REQUIRE(events.size() == 2);
  CHECK(events[0].m_text == "Hello");
  CHECK(events[1].m_kind == chat_event_kind::stop);
}

TEST_CASE("a response without any event yields a finished stream", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, ""));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  REQUIRE(events.size() == 1);
  CHECK(events.front().m_kind == chat_event_kind::stop);
  CHECK(events.front().m_reason == "stop");
}

TEST_CASE("the stream survives a chunked body", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub_response response;
  response.m_status = 200;
  response.m_chunked = true;
  auto const whole = success_stream();
  // Nine-byte chunks keep the body split across reads without depending on how the stub spells a
  // chunk length; the wider chunks are covered by the http_client case.
  for (std::size_t offset = 0; offset < whole.size(); offset += 9) {
    response.m_chunks.push_back(whole.substr(offset, 9));
  }
  stub.m_answers.push_back(response);
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(kinds_of(outcome->value().m_events) == std::vector<chat_event_kind>{chat_event_kind::text,
                                                                            chat_event_kind::text,
                                                                            chat_event_kind::stop});
}

TEST_CASE("the stream survives chunks wider than one hex digit", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub_response response;
  response.m_status = 200;
  response.m_chunked = true;
  auto const whole = success_stream();
  // A chunk of ten bytes or more is spelled with two hex digits, so this is the width where a stub
  // that wrote the length as decimal would corrupt the body; the stream has to reassemble it.
  constexpr std::size_t chunk_size = 32;
  for (std::size_t offset = 0; offset < whole.size(); offset += chunk_size) {
    response.m_chunks.push_back(whole.substr(offset, chunk_size));
  }
  stub.m_answers.push_back(response);
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  REQUIRE(kinds_of(events) == std::vector<chat_event_kind>{chat_event_kind::text,
                                                           chat_event_kind::text,
                                                           chat_event_kind::stop});
  // The body arrived in pieces, so the reassembled text is what proves the chunk framing survived.
  std::string reassembled;
  for (auto const& event : events) {
    reassembled.append(event.m_text);
  }
  CHECK(reassembled == "Hello world");
  CHECK(events[2].m_reason == "stop");
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a refused connection maps to upstream", "[qoder][chat][client]") {
  asio::io_context io;
  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  // Port 1 is never served on loopback, so the connection is refused instead of answered.
  auto site =
      qoderz::config::apply_loopback_override(make_site_without_server(), "http://127.0.0.1:1");
  REQUIRE(site.has_value());

  upstream_request request{
      .m_site = std::move(*site),
      .m_account = make_account(),
      .m_body_json = R"({"request_id":"r1"})",
      .m_model_key = "m",
      .m_model_source = "",
  };

  auto outcome = run_awaitable(io, client.open(request));
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
}

TEST_CASE("the peeked first event is delivered, not lost", "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  REQUIRE(events.size() == 3);
  CHECK(events[0].m_text == "Hello");
  CHECK(events[1].m_text == " world");
  CHECK(events[2].m_kind == chat_event_kind::stop);
}

TEST_CASE("a text tool call spanning chunks reaches the caller as one call",
          "[qoder][chat][client]") {
  asio::io_context io;
  scripted_stub stub;
  std::string body;
  body.append(frame(R"({"choices":[{"delta":{"content":"<tool_call><function=read>"}}]})"));
  body.append(frame(R"({"choices":[{"delta":{"content":"<parameter=path>a.go"}}]})"));
  body.append(
      frame(R"({"choices":[{"delta":{"content":"</parameter></function></tool_call>"}}]})"));
  stub.m_answers.push_back(text_response(200, std::move(body)));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, retry_options{}};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  auto const& events = outcome->value().m_events;
  REQUIRE(events.size() == 2);
  CHECK(events[0].m_kind == chat_event_kind::tool_start);
  CHECK(events[0].m_name == "read");
  CHECK(events[0].m_arguments == R"({"path":"a.go"})");
  CHECK(events[1].m_kind == chat_event_kind::stop);
  CHECK(events[1].m_reason == "tool_calls");
}
