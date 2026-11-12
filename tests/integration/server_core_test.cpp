#include "server/config.hpp"
#include "server/router.hpp"
#include "support/async_test.hpp"
#include "support/server_harness.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using qoderz::error_kind;
using qoderz::json;
using qoderz::server::server;
using qoderz::server::server_config;
using qoderz::server::server_dependencies;
using qoderz::test::bearer;
using qoderz::test::build_request;
using qoderz::test::exchange;
using qoderz::test::fake_chat;
using qoderz::test::fake_key;
using qoderz::test::header_of;
using qoderz::test::http_result;
using qoderz::test::one_request;
using qoderz::test::run_awaitable;
using qoderz::test::server_harness;

} // namespace

TEST_CASE("server binds port 0, reports the port and stops", "[server][integration]") {
  server_harness scope;
  CHECK(scope.m_port != 0);

  server_config defaults;
  CHECK(defaults.m_listen_host == "127.0.0.1");
  CHECK(defaults.m_port == 8788);
  CHECK(defaults.m_site_id == "qoder-cn");
  CHECK(defaults.m_max_body_bytes == 32 * 1024 * 1024);
  CHECK_FALSE(defaults.m_allow_public);
  CHECK_FALSE(defaults.m_api_key.has_value());

  auto health = scope.request("GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);

  scope.m_server->stop();
  // The acceptor is closed on the io_context, so a further connection must be refused.
  auto after = scope.request("GET", "/healthz");
  CHECK_FALSE(after.has_value());
}

TEST_CASE("health answers without authentication", "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok, std::string{fake_key}};
  auto health = scope.request("GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);
  CHECK(health->m_body == R"({"status":"ok"})");
  CHECK(header_of(*health, "Content-Type") == "application/json");
}

TEST_CASE("models is open without a key and answers the catalogue", "[server][integration]") {
  server_harness scope;
  auto models = scope.request("GET", "/v1/models");
  REQUIRE(models.has_value());
  CHECK(models->m_status == 200);

  auto const parsed = json::parse(models->m_body, nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("object") == "list");
  CHECK(parsed.at("data").at(0).at("id") == "ultimate");
}

TEST_CASE("models requires the configured key", "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok, std::string{fake_key}};

  auto missing = scope.request("GET", "/v1/models");
  REQUIRE(missing.has_value());
  CHECK(missing->m_status == 401);
  auto const body = json::parse(missing->m_body, nullptr, false);
  CHECK(body.at("error").at("type") == "authentication_error");
  CHECK(body.at("error").at("code") == 401);

  auto wrong = scope.request("GET", "/v1/models", {bearer("wrong-key-not-real")});
  REQUIRE(wrong.has_value());
  CHECK(wrong->m_status == 401);

  auto good = scope.request("GET", "/v1/models", {bearer(fake_key)});
  REQUIRE(good.has_value());
  CHECK(good->m_status == 200);
}

TEST_CASE("an unknown endpoint is 404 and never authenticated", "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok, std::string{fake_key}};
  auto missing = scope.request("GET", "/nope");
  REQUIRE(missing.has_value());
  CHECK(missing->m_status == 404);
  auto const body = json::parse(missing->m_body, nullptr, false);
  CHECK(body.at("error").at("message") ==
        "unknown endpoint; use /v1/models or /v1/chat/completions");
  CHECK(body.at("error").at("type") == "invalid_request_error");
  CHECK(body.at("error").at("code") == 404);
}

TEST_CASE("a known path with the wrong method is 405 with Allow", "[server][integration]") {
  server_harness scope;
  auto models = scope.request("POST", "/v1/models", {}, "{}");
  REQUIRE(models.has_value());
  CHECK(models->m_status == 405);
  CHECK(header_of(*models, "Allow") == "GET");
  CHECK(json::parse(models->m_body, nullptr, false).at("error").at("code") == 405);

  auto chat = scope.request("GET", "/v1/chat/completions");
  REQUIRE(chat.has_value());
  CHECK(chat->m_status == 405);
  CHECK(header_of(*chat, "Allow") == "POST");

  auto health = scope.request("POST", "/healthz", {}, "{}");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 405);
  CHECK(header_of(*health, "Allow") == "GET");
  CHECK(scope.m_chat.m_calls == 0);
}

TEST_CASE("a wrong method on a /v1 path is authenticated before it is answered",
          "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok, std::string{fake_key}};
  auto models = scope.request("POST", "/v1/models", {}, "{}");
  REQUIRE(models.has_value());
  CHECK(models->m_status == 401);

  auto chat = scope.request("GET", "/v1/chat/completions");
  REQUIRE(chat.has_value());
  CHECK(chat->m_status == 401);

  auto allowed = scope.request("POST", "/v1/models", {bearer(fake_key)}, "{}");
  REQUIRE(allowed.has_value());
  CHECK(allowed->m_status == 405);
  CHECK(header_of(*allowed, "Allow") == "GET");

  // `/healthz` is not behind the key, so its 405 comes without one.
  auto health = scope.request("POST", "/healthz", {}, "{}");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 405);
  CHECK(header_of(*health, "Allow") == "GET");
}

TEST_CASE("a body over the limit is 413 and closes the connection", "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok, std::nullopt, 1024};
  std::string const body(4096, 'x');
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, body);
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 413);
  auto const parsed = json::parse(outcome->m_body, nullptr, false);
  CHECK(parsed.at("error").at("type") == "invalid_request_error");
  CHECK(parsed.at("error").at("code") == 413);
  CHECK(parsed.at("error").at("message") == "the request body is larger than the server accepts");
  CHECK(header_of(*outcome, "Connection") == "close");
  CHECK(scope.m_chat.m_calls == 0);
}

TEST_CASE("a chat handler can answer with one JSON body", "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok};
  auto outcome =
      scope.request("POST", "/v1/chat/completions", {}, R"({"model":"ultimate","messages":[]})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(header_of(*outcome, "Content-Type") == "application/json");
  CHECK(outcome->m_body == R"({"object":"chat.completion","ok":true})");
  CHECK(scope.m_chat.m_calls == 1);
  CHECK(scope.m_chat.m_last.m_body == R"({"model":"ultimate","messages":[]})");
  CHECK_FALSE(scope.m_chat.m_last.m_stream);
  CHECK(scope.m_chat.m_last.m_session_id.empty());
}

TEST_CASE("the router passes the session header and the stream flag to the handler",
          "[server][integration]") {
  server_harness scope{fake_chat::mode::json_ok};
  auto outcome = scope.request("POST", "/v1/chat/completions", {{"X-Session-Id", "  session-42  "}},
                               R"({"model":"ultimate","stream":true})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(scope.m_chat.m_last.m_stream);
  CHECK(scope.m_chat.m_last.m_session_id == "session-42");

  auto preferred = scope.request("POST", "/v1/chat/completions",
                                 {{"X-Qoder2Api-Session", "primary"}, {"X-Session-Id", "fallback"}},
                                 R"({"stream":"true"})");
  REQUIRE(preferred.has_value());
  CHECK(scope.m_chat.m_last.m_session_id == "primary");
  // Only a JSON boolean true counts as a stream request.
  CHECK_FALSE(scope.m_chat.m_last.m_stream);
}

TEST_CASE("a streaming handler writes chunked SSE frames", "[server][integration]") {
  server_harness scope{fake_chat::mode::sse_ok};
  auto outcome =
      scope.request("POST", "/v1/chat/completions", {}, R"({"stream":true,"messages":[]})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(header_of(*outcome, "Content-Type") == "text/event-stream");
  CHECK(header_of(*outcome, "Cache-Control") == "no-cache");
  CHECK(header_of(*outcome, "X-Accel-Buffering") == "no");
  CHECK(outcome->m_chunked);
  CHECK(outcome->m_body == "data: {\"n\":1}\n\ndata: [DONE]\n\n");
}

TEST_CASE("a handler error before the response starts becomes an HTTP error",
          "[server][integration]") {
  server_harness scope{fake_chat::mode::error_before_start};
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, R"({"messages":[]})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 400);
  auto const parsed = json::parse(outcome->m_body, nullptr, false);
  CHECK(parsed.at("error").at("type") == "invalid_request_error");
  CHECK(parsed.at("error").at("message") == "messages is required");
  CHECK(header_of(*outcome, "Content-Type") == "application/json");
}

TEST_CASE("a handler error after the stream started becomes one error frame",
          "[server][integration]") {
  server_harness scope{fake_chat::mode::error_after_start};
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, R"({"stream":true})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 200);
  CHECK(outcome->m_chunked);
  CHECK(outcome->m_body.starts_with("data: {\"n\":1}\n\n"));
  CHECK(outcome->m_body.find("data: {\"error\"") != std::string::npos);
  CHECK(outcome->m_body.find("\"code\":502") != std::string::npos);
  CHECK(outcome->m_body.find("[DONE]") == std::string::npos);
}

TEST_CASE("an SSE answer closes the connection even after its last chunk",
          "[server][integration]") {
  server_harness scope{fake_chat::mode::sse_ok};
  // The first answer has to be a real one, otherwise a missing connection would look like a pass.
  bool answered = false;
  bool reused = false;
  auto work = [&]() -> asio::awaitable<bool> {
    auto executor = co_await asio::this_coro::executor;
    beast::tcp_stream stream{executor};
    beast::error_code ec;
    stream.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), scope.m_port}, ec);
    if (ec) {
      co_return false;
    }
    beast::flat_buffer buffer;
    auto streamed = co_await exchange(
        stream, build_request("POST", "/v1/chat/completions", {}, R"({"stream":true})", true),
        buffer);
    if (!streamed.has_value()) {
      co_return false;
    }
    answered = streamed->m_status == 200 && header_of(*streamed, "Connection") == "close";
    // The head promised `Connection: close`, so the next request must find no connection at all.
    auto after = co_await exchange(stream, build_request("GET", "/healthz", {}, {}, true), buffer);
    reused = after.has_value();
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return true;
  };
  auto ran = run_awaitable(scope.m_io, work());
  REQUIRE(ran.has_value());
  REQUIRE(*ran);
  CHECK(answered);
  CHECK_FALSE(reused);
}

TEST_CASE("two requests share one keep-alive connection", "[server][integration]") {
  server_harness scope;
  auto work = [&]() -> asio::awaitable<std::optional<http_result>> {
    auto executor = co_await asio::this_coro::executor;
    beast::tcp_stream stream{executor};
    beast::error_code ec;
    stream.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), scope.m_port}, ec);
    if (ec) {
      co_return std::nullopt;
    }
    beast::flat_buffer buffer;
    auto first = co_await exchange(stream, build_request("GET", "/healthz", {}, {}, true), buffer);
    if (!first.has_value()) {
      co_return std::nullopt;
    }
    auto second =
        co_await exchange(stream, build_request("GET", "/v1/models", {}, {}, false), buffer);
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    if (!second.has_value() || first->m_status != 200 || second->m_status != 200) {
      co_return std::nullopt;
    }
    co_return second;
  };
  auto outcome = run_awaitable(scope.m_io, work());
  REQUIRE(outcome.has_value());
  CHECK((*outcome)->m_status == 200);
}

TEST_CASE("a client that disconnects mid-request does not disturb the server",
          "[server][integration]") {
  server_harness scope;
  {
    beast::error_code ec;
    asio::ip::tcp::socket socket{scope.m_io};
    socket.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), scope.m_port}, ec);
    REQUIRE_FALSE(ec);
    std::string const partial = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Len";
    asio::write(socket, asio::buffer(partial), ec);
    beast::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
  }
  auto health = scope.request("GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);
}

TEST_CASE("a non-loopback listener without a key is refused", "[server][integration]") {
  server_config config;
  config.m_listen_host = "0.0.0.0";
  config.m_port = 0;
  auto refused = qoderz::server::validate_config(config);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().m_kind == error_kind::configuration);
  CHECK(refused.error().m_message.find("--allow-public") != std::string::npos);

  config.m_allow_public = true;
  CHECK(qoderz::server::validate_config(config).has_value());

  config.m_allow_public = false;
  config.m_api_key = std::string{fake_key};
  CHECK(qoderz::server::validate_config(config).has_value());

  // A loopback listener never needs a key.
  server_config loopback;
  loopback.m_listen_host = "127.0.0.1";
  CHECK(qoderz::server::validate_config(loopback).has_value());
  loopback.m_listen_host = "localhost";
  CHECK(qoderz::server::validate_config(loopback).has_value());
  loopback.m_listen_host = "::1";
  CHECK(qoderz::server::validate_config(loopback).has_value());
}

TEST_CASE("a refused configuration makes start fail before binding", "[server][integration]") {
  asio::io_context io;
  fake_chat chat{fake_chat::mode::json_ok};
  server_harness scope;
  server_config config;
  config.m_listen_host = "0.0.0.0";
  config.m_port = 0;
  server refused{io, std::move(config),
                 server_dependencies{.m_chat = chat, .m_models = *scope.m_models}};
  auto started = refused.start();
  REQUIRE_FALSE(started.has_value());
  CHECK(started.error().m_kind == error_kind::configuration);
  CHECK(refused.port() == 0);
}

TEST_CASE("graceful shutdown lets an in-flight request finish", "[server][integration]") {
  server_harness scope{fake_chat::mode::slow_json, std::nullopt, 32 * 1024 * 1024,
                       std::chrono::milliseconds{120}};
  auto work = [&]() -> asio::awaitable<std::optional<http_result>> {
    std::optional<http_result> answered;
    asio::co_spawn(
        scope.m_io,
        [&]() -> asio::awaitable<void> {
          answered = co_await one_request(
              scope.m_port, build_request("POST", "/v1/chat/completions", {}, "{}", false));
          co_return;
        },
        asio::detached);
    asio::steady_timer timer{co_await asio::this_coro::executor, std::chrono::milliseconds{40}};
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
    scope.m_server->stop();
    co_await scope.m_server->drain();
    co_return answered;
  };
  auto outcome = run_awaitable(scope.m_io, work());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK((*outcome)->m_status == 200);

  auto refused = scope.request("GET", "/healthz");
  CHECK_FALSE(refused.has_value());
}

TEST_CASE("a quota handler answer never carries Retry-After", "[server][integration]") {
  server_harness scope{fake_chat::mode::error_quota};
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, R"({"messages":[]})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 429);
  auto const parsed = json::parse(outcome->m_body, nullptr, false);
  CHECK(parsed.at("error").at("type") == "insufficient_quota");
  CHECK(parsed.at("error").at("code") == 429);
  // Only a 503 carries the hint, so a quota answer must not.
  CHECK(header_of(*outcome, "Retry-After").empty());
}

TEST_CASE("a busy handler answer is 503 with Retry-After", "[server][integration]") {
  server_harness scope{fake_chat::mode::error_busy};
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, R"({"messages":[]})");
  REQUIRE(outcome.has_value());
  CHECK(outcome->m_status == 503);
  auto const parsed = json::parse(outcome->m_body, nullptr, false);
  CHECK(parsed.at("error").at("type") == "server_busy");
  CHECK(parsed.at("error").at("code") == 503);
  CHECK(header_of(*outcome, "Retry-After") == "7");
}

TEST_CASE("a canceled handler answer writes no body at all", "[server][integration]") {
  server_harness scope{fake_chat::mode::error_canceled};
  auto outcome = scope.request("POST", "/v1/chat/completions", {}, R"({"messages":[]})");
  CHECK_FALSE(outcome.has_value());

  auto health = scope.request("GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);
}

TEST_CASE("the destructor closes the listener", "[server][integration]") {
  server_harness scope;
  auto& io = scope.m_io;
  fake_chat chat{fake_chat::mode::json_ok};

  server_config config;
  config.m_listen_host = "127.0.0.1";
  config.m_port = 0;

  auto listener = std::make_unique<server>(
      io, std::move(config), server_dependencies{.m_chat = chat, .m_models = *scope.m_models});
  auto started = listener->start();
  REQUIRE(started.has_value());
  auto const port = *started;
  // The accept loop is what keeps a reference to the state, so it has to be running for the case to
  // mean anything.
  asio::co_spawn(io, listener->run(), asio::detached);
  std::jthread pumping{[&io] { io.run(); }};
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  listener.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  beast::error_code ec;
  asio::ip::tcp::socket probe{io};
  probe.connect(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), port}, ec);
  CHECK(ec);
  beast::error_code ignored;
  probe.close(ignored);

  // Reuse address keeps the TIME_WAIT of the probe from deciding the answer, so a failed bind can
  // only mean the listener is still open.
  asio::ip::tcp::acceptor again{io};
  beast::error_code bound;
  again.open(asio::ip::tcp::v4(), bound);
  again.set_option(asio::socket_base::reuse_address{true}, bound);
  again.bind(asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), port}, bound);
  CHECK_FALSE(bound);

  io.stop();
  pumping.join();
}
