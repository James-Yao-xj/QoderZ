#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "support/async_test.hpp"
#include "support/stub_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using qoderz::error_kind;
using qoderz::net::header_value;
using qoderz::net::http_client;
using qoderz::net::http_client_options;
using qoderz::net::http_header;
using qoderz::net::http_request;
using qoderz::net::http_response;
using qoderz::net::http_stream;
using qoderz::test::run_awaitable;
using qoderz::test::stub_handler;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

using namespace std::chrono_literals;

stub_handler fixed(int status, std::string body, std::vector<http_header> headers = {}) {
  return [status, body = std::move(body),
          headers = std::move(headers)](stub_request const&) -> asio::awaitable<stub_response> {
    stub_response response;
    response.m_status = status;
    response.m_body = body;
    response.m_headers = headers;
    co_return response;
  };
}

/// Requests are made lazily by the test itself so the stub and the client share one event loop.
struct fixture {
  asio::io_context m_io;
  http_client_options m_options;

  explicit fixture(http_client_options options = {}) : m_options{options} {}

  qoderz::result<http_response> get(stub_server& server, std::string target) {
    http_client client{m_io, m_options};
    http_request request;
    request.m_method = "GET";
    request.m_url = server.origin() + std::move(target);
    auto outcome = run_awaitable(m_io, client.send(request));
    if (!outcome.has_value()) {
      return std::unexpected(qoderz::make_error(error_kind::internal, "the awaitable stalled"));
    }
    return std::move(*outcome);
  }
};

} // namespace

TEST_CASE("send returns the status, headers and body of a GET", "[common][net][http_client]") {
  asio::io_context io;
  http_client_options options;
  stub_server server{io, fixed(200, "hello there",
                               {http_header{.m_name = "Content-Type", .m_value = "text/plain"}})};
  REQUIRE(server.start().has_value());

  http_client client{io, options};
  http_request request;
  request.m_url = server.origin() + "/greeting?name=ada";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_status == 200);
  CHECK(outcome->value().m_body == "hello there");
  auto const* content_type = header_value(outcome->value().m_headers, "content-type");
  REQUIRE(content_type != nullptr);
  CHECK(*content_type == "text/plain");
}

TEST_CASE("send puts the method, request headers and body on the wire",
          "[common][net][http_client]") {
  asio::io_context io;
  stub_request seen;
  bool captured = false;
  stub_server server{io, [&](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen = request;
                       captured = true;
                       stub_response response;
                       response.m_status = 201;
                       response.m_body = "created";
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_method = "POST";
  request.m_url = server.origin() + "/items";
  request.m_headers.push_back(http_header{.m_name = "X-Test", .m_value = "test-token-not-real"});
  request.m_body = R"({"name":"ada"})";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_status == 201);
  REQUIRE(captured);
  CHECK(seen.m_method == "POST");
  CHECK(seen.m_target == "/items");
  CHECK(seen.m_body == R"({"name":"ada"})");
  auto const* test_header = header_value(seen.m_headers, "X-Test");
  REQUIRE(test_header != nullptr);
  CHECK(*test_header == "test-token-not-real");
}

TEST_CASE("send treats 4xx and 5xx as ordinary responses", "[common][net][http_client]") {
  for (auto const status : {400, 401, 404, 429, 500, 503}) {
    asio::io_context io;
    stub_server server{io, fixed(status, "nope")};
    REQUIRE(server.start().has_value());
    http_client client{io};
    http_request request;
    request.m_url = server.origin() + "/x";
    auto outcome = run_awaitable(io, client.send(request));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK(outcome->value().m_status == status);
    CHECK(outcome->value().m_body == "nope");
  }
}

TEST_CASE("open returns a stream over a Content-Length body", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, "0123456789")};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/ten";
  auto outcome = run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<std::string>> {
    auto stream = co_await client.open(request);
    if (!stream) {
      co_return std::unexpected(stream.error());
    }
    std::string body;
    for (;;) {
      auto chunk = co_await (*stream)->read_some();
      if (!chunk) {
        co_return std::unexpected(chunk.error());
      }
      if (!chunk->has_value()) {
        co_return body;
      }
      body.append(chunk->value());
    }
  }());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value() == "0123456789");
}

TEST_CASE("open decodes a chunked body chunk by chunk", "[common][net][http_client]") {
  asio::io_context io;
  stub_response response;
  response.m_chunked = true;
  response.m_chunks = {"alpha", "beta", "gamma"};
  stub_server server{io, [response](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/chunks";
  auto outcome =
      run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<std::vector<std::string>>> {
        auto stream = co_await client.open(request);
        if (!stream) {
          co_return std::unexpected(stream.error());
        }
        std::vector<std::string> chunks;
        for (;;) {
          auto chunk = co_await (*stream)->read_some();
          if (!chunk) {
            co_return std::unexpected(chunk.error());
          }
          if (!chunk->has_value()) {
            co_return chunks;
          }
          chunks.push_back(chunk->value());
        }
      }());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().size() >= 3);
  std::string joined;
  for (auto const& chunk : outcome->value()) {
    joined.append(chunk);
  }
  CHECK(joined == "alphabetagamma");
}

TEST_CASE("send buffers a chunked body from a stub", "[common][net][http_client]") {
  asio::io_context io;
  stub_response response;
  response.m_chunked = true;
  response.m_chunks = {"one", "-", "two"};
  stub_server server{io, [response](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/chunked";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_body == "one-two");
}

TEST_CASE("an empty body reports end of stream immediately", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(204, "")};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/nothing";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_status == 204);
  CHECK(outcome->value().m_body.empty());
}

TEST_CASE("a peer that closes mid-body is an upstream error", "[common][net][http_client]") {
  asio::io_context io;
  stub_response response;
  response.m_status = 200;
  response.m_body = "0123456789abcdef";
  response.m_truncate_body = true;
  stub_server server{io, [response](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/truncated";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("127.0.0.1") != std::string::npos);
  CHECK(outcome->error().m_message.find("abcdef") == std::string::npos);
}

TEST_CASE("sequential requests to one origin accept a single connection",
          "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, "ok")};
  REQUIRE(server.start().has_value());

  http_client client{io};
  for (int round = 0; round < 3; ++round) {
    http_request request;
    request.m_url = server.origin() + "/again";
    auto outcome = run_awaitable(io, client.send(request));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
  }
  CHECK(server.request_count() == 3);
  CHECK(server.connection_count() == 1);
}

TEST_CASE("Connection: close makes the client open a new connection",
          "[common][net][http_client]") {
  asio::io_context io;
  stub_response response;
  response.m_status = 200;
  response.m_body = "bye";
  response.m_close_after = true;
  stub_server server{io, [response](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  for (int round = 0; round < 2; ++round) {
    http_request request;
    request.m_url = server.origin() + "/closing";
    auto outcome = run_awaitable(io, client.send(request));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK(outcome->value().m_body == "bye");
  }
  CHECK(server.request_count() == 2);
  CHECK(server.connection_count() == 2);
}

TEST_CASE("a header delay past the timeout is an upstream timeout", "[common][net][http_client]") {
  asio::io_context io;
  stub_response slow;
  slow.m_status = 200;
  slow.m_body = "late";
  slow.m_delay_before_headers = 2s;
  stub_server server{
      io, [slow](stub_request const&) -> asio::awaitable<stub_response> { co_return slow; }};
  REQUIRE(server.start().has_value());

  http_client_options options;
  options.m_request_timeout = 100ms;
  http_client client{io, options};
  http_request request;
  request.m_url = server.origin() + "/slow";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("127.0.0.1") != std::string::npos);
  CHECK(outcome->error().m_message.find("timeout") != std::string::npos);
}

TEST_CASE("a refused connection is an upstream connect error", "[common][net][http_client]") {
  asio::io_context io;
  http_client_options options;
  options.m_connect_timeout = 500ms;
  http_client client{io, options};

  std::uint16_t port = 0;
  {
    stub_server probe{io, fixed(200, "gone")};
    auto started = probe.start();
    REQUIRE(started.has_value());
    port = *started;
    probe.stop();
  }

  http_request request;
  request.m_url = "http://127.0.0.1:" + std::to_string(port) + "/nowhere";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("connect") != std::string::npos);
  CHECK(outcome->error().m_message.find("127.0.0.1") != std::string::npos);
}

TEST_CASE("a buffered body above the cap is an upstream error", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, std::string(4096, 'x'))};
  REQUIRE(server.start().has_value());

  http_client_options options;
  options.m_max_buffered_body = 1024;
  http_client client{io, options};
  http_request request;
  request.m_url = server.origin() + "/large";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_message.find("larger than allowed") != std::string::npos);
}

TEST_CASE("a stream keeps its connection until the body ends", "[common][net][http_client]") {
  asio::io_context io;
  stub_response first;
  first.m_status = 200;
  first.m_body = "first-body";
  stub_server server{
      io, [first](stub_request const&) -> asio::awaitable<stub_response> { co_return first; }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/first";

  auto dropped = run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<void>> {
    auto stream = co_await client.open(request);
    if (!stream) {
      co_return std::unexpected(stream.error());
    }
    co_return qoderz::result<void>{};
  }());
  REQUIRE(dropped.has_value());

  auto again = run_awaitable(io, client.send(request));
  REQUIRE(again.has_value());
  REQUIRE(again->has_value());
  CHECK(again->value().m_body == "first-body");
  CHECK(server.connection_count() == 2);
}

TEST_CASE("concurrent requests to one origin do not mix bodies", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const& request) -> asio::awaitable<stub_response> {
                       stub_response response;
                       response.m_status = 200;
                       response.m_body = "body-for" + request.m_target;
                       response.m_delay_before_headers = 20ms;
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  auto outcome =
      run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<std::vector<std::string>>> {
        std::vector<asio::awaitable<qoderz::result<http_response>>> pending;
        auto one = [&]() -> asio::awaitable<qoderz::result<http_response>> {
          http_request request;
          request.m_url = server.origin() + "/a";
          co_return co_await client.send(request);
        };
        auto two = [&]() -> asio::awaitable<qoderz::result<http_response>> {
          http_request request;
          request.m_url = server.origin() + "/b";
          co_return co_await client.send(request);
        };
        std::vector<std::string> bodies;
        auto first = co_await one();
        if (!first) {
          co_return std::unexpected(first.error());
        }
        bodies.push_back(first->m_body);
        auto second = co_await two();
        if (!second) {
          co_return std::unexpected(second.error());
        }
        bodies.push_back(second->m_body);
        co_return bodies;
      }());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value()[0] == "body-for/a");
  CHECK(outcome->value()[1] == "body-for/b");
}

TEST_CASE("close drops idle connections before the next request", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, "fine")};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/one";
  REQUIRE(run_awaitable(io, client.send(request))->has_value());
  CHECK(server.connection_count() == 1);

  client.close();
  auto second = run_awaitable(io, client.send(request));
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  CHECK(second->value().m_body == "fine");
  CHECK(server.connection_count() == 2);
}

TEST_CASE("a request to a malformed URL fails before touching the network",
          "[common][net][http_client]") {
  asio::io_context io;
  http_client client{io};
  http_request request;
  request.m_url = "ftp://example.com/x";
  auto outcome = run_awaitable(io, client.send(request));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::bad_request);
}
TEST_CASE("chunk sizes above nine are written in hexadecimal", "[common][net][http_client]") {
  asio::io_context io;
  stub_response response;
  response.m_chunked = true;
  response.m_chunks = {"0123456789", "abcdefghijklmnopqrstuvwxyz", "0123456789abcdef"};
  stub_server server{io, [response](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/wide-chunks";
  auto outcome =
      run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<std::vector<std::string>>> {
        auto stream = co_await client.open(request);
        if (!stream) {
          co_return std::unexpected(stream.error());
        }
        std::vector<std::string> chunks;
        for (;;) {
          auto chunk = co_await (*stream)->read_some();
          if (!chunk) {
            co_return std::unexpected(chunk.error());
          }
          if (!chunk->has_value()) {
            co_return chunks;
          }
          chunks.push_back(chunk->value());
        }
      }());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  std::string joined;
  for (auto const& chunk : outcome->value()) {
    joined.append(chunk);
  }
  CHECK(joined == "0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdef");
}
