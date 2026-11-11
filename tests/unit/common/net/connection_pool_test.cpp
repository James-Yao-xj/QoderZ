#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "support/async_test.hpp"
#include "support/stub_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using qoderz::error_kind;
using qoderz::net::http_client;
using qoderz::net::http_client_options;
using qoderz::net::http_header;
using qoderz::net::http_request;
using qoderz::test::run_awaitable;
using qoderz::test::stub_handler;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

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

} // namespace

TEST_CASE("an idle connection past its TTL is not reused", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, "ok")};
  REQUIRE(server.start().has_value());

  http_client_options options;
  options.m_idle_ttl = std::chrono::seconds{0};
  http_client client{io, options};
  http_request request;
  request.m_url = server.origin() + "/ttl";
  for (int round = 0; round < 2; ++round) {
    auto outcome = run_awaitable(io, client.send(request));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
  }
  CHECK(server.connection_count() == 2);
}

TEST_CASE("the idle cap closes a connection instead of pooling it", "[common][net][http_client]") {
  asio::io_context io;
  stub_server server{io, fixed(200, "ok")};
  REQUIRE(server.start().has_value());

  http_client_options options;
  options.m_max_idle_per_origin = 0;
  http_client client{io, options};
  http_request request;
  request.m_url = server.origin() + "/cap";
  for (int round = 0; round < 2; ++round) {
    auto outcome = run_awaitable(io, client.send(request));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
  }
  CHECK(server.connection_count() == 2);
}

TEST_CASE("a pooled connection the peer abandoned is retried once", "[common][net][http_client]") {
  asio::io_context io;
  int calls = 0;
  stub_server server{io, [&calls](stub_request const&) -> asio::awaitable<stub_response> {
                       ++calls;
                       stub_response response;
                       response.m_status = 200;
                       response.m_body = "ok";
                       // The first answer is a normal keep-alive response, but the peer drops the
                       // socket right afterwards, so the pooled connection is dead on reuse.
                       response.m_close_silently = calls == 1;
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/stale";

  auto first = run_awaitable(io, client.send(request));
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  CHECK(first->value().m_body == "ok");
  CHECK(server.connection_count() == 1);

  auto second = run_awaitable(io, client.send(request));
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  CHECK(second->value().m_body == "ok");
  CHECK(server.connection_count() == 2);
  CHECK(calls == 2);
}

TEST_CASE("a peer that dies inside the header block is not retried", "[common][net][http_client]") {
  asio::io_context io;
  int calls = 0;
  stub_server server{io, [&calls](stub_request const&) -> asio::awaitable<stub_response> {
                       ++calls;
                       stub_response response;
                       response.m_status = 200;
                       response.m_body = "ok";
                       // The second request rides the pooled connection and gets half a header.
                       response.m_truncate_header = calls == 2;
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  http_client client{io};
  http_request request;
  request.m_url = server.origin() + "/broken-header";

  auto first = run_awaitable(io, client.send(request));
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());

  auto second = run_awaitable(io, client.send(request));
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->has_value());
  CHECK(second->error().m_kind == error_kind::upstream);
  // Response bytes arrived, so the client must report the failure instead of resending.
  CHECK(server.request_count() == 2);
  CHECK(server.connection_count() == 1);
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
