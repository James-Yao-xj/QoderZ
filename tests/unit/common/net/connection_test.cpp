#include "common/net/detail/connection.hpp"
#include "common/net/url.hpp"
#include "support/async_test.hpp"
#include "support/stub_server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <utility>

namespace asio = boost::asio;

using namespace std::chrono_literals;
using qoderz::net::http_request;
using qoderz::net::parse_url;
using qoderz::net::detail::connection;
using qoderz::test::run_awaitable;
using qoderz::test::stub_handler;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;

namespace {

stub_handler answering(std::string body) {
  return [body = std::move(body)](stub_request const&) -> asio::awaitable<stub_response> {
    stub_response response;
    response.m_status = 200;
    response.m_body = body;
    co_return response;
  };
}

} // namespace

TEST_CASE("a connection clears response_started before its next request",
          "[common][net][connection]") {
  asio::io_context io;
  stub_server server{io, answering("ok")};
  REQUIRE(server.start().has_value());

  auto const url = parse_url(server.origin() + "/flag");
  REQUIRE(url.has_value());
  http_request request;
  request.m_url = server.origin() + "/flag";

  auto outcome = run_awaitable(io, [&]() -> asio::awaitable<qoderz::result<std::pair<bool, bool>>> {
    auto handle = co_await connection::open(*url, 1s);
    if (!handle) {
      co_return std::unexpected(handle.error());
    }
    auto& socket = *handle;

    auto sent = co_await socket->send(*url, request, 1s);
    if (!sent) {
      co_return std::unexpected(sent.error());
    }
    auto header = co_await socket->read_header(1024, 1s);
    if (!header) {
      co_return std::unexpected(header.error());
    }

    // Drain the body so the greeting is a complete, reusable exchange.
    for (;;) {
      auto chunk = co_await socket->read_some(1s);
      if (!chunk) {
        co_return std::unexpected(chunk.error());
      }
      if (!chunk->has_value()) {
        break;
      }
    }
    bool const after_response = socket->response_started();

    // The next request owns the flag: it has to describe this exchange, not the last one. A
    // stale `true` here would suppress the retry a dead pooled connection is entitled to.
    auto again = co_await socket->send(*url, request, 1s);
    (void)again;
    co_return std::pair{after_response, socket->response_started()};
  }());
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().first);
  CHECK_FALSE(outcome->value().second);
}
