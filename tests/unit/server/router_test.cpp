#include "server/error_writer.hpp"
#include "server/router.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace {

using qoderz::server::match_route;
using qoderz::server::path_of;
using qoderz::server::requires_authentication;
using qoderz::server::route_kind;

} // namespace

TEST_CASE("The three documented routes match their method", "[server][router]") {
  CHECK(match_route("GET", "/healthz").m_kind == route_kind::health);
  CHECK(match_route("GET", "/v1/models").m_kind == route_kind::models);
  CHECK(match_route("POST", "/v1/chat/completions").m_kind == route_kind::chat);
}

TEST_CASE("A known path with the wrong method answers 405 and names the method",
          "[server][router]") {
  auto const models = match_route("POST", "/v1/models");
  CHECK(models.m_kind == route_kind::wrong_method);
  CHECK(models.m_allow == "GET");

  auto const chat = match_route("GET", "/v1/chat/completions");
  CHECK(chat.m_kind == route_kind::wrong_method);
  CHECK(chat.m_allow == "POST");

  auto const health = match_route("DELETE", "/healthz");
  CHECK(health.m_kind == route_kind::wrong_method);
  CHECK(health.m_allow == "GET");

  for (auto const method : {"PUT", "PATCH", "HEAD", "OPTIONS", "TRACE", "get"}) {
    CHECK(match_route(method, "/v1/models").m_kind == route_kind::wrong_method);
    CHECK(match_route(method, "/v1/models").m_allow == "GET");
    CHECK(match_route(method, "/v1/chat/completions").m_allow == "POST");
    CHECK(match_route(method, "/healthz").m_allow == "GET");
  }
}

TEST_CASE("An unknown path is 404 whatever the method", "[server][router]") {
  for (auto const path : {"/", "/v1", "/v1/models/", "/v1/model", "/v2/models", "/health",
                          "/v1/chat/completion", "/v1/models/extra"}) {
    CHECK(match_route("GET", path).m_kind == route_kind::not_found);
    CHECK(match_route("POST", path).m_kind == route_kind::not_found);
    CHECK(match_route("GET", path).m_allow.empty());
  }
}

TEST_CASE("Only the two /v1 endpoints sit behind the API key", "[server][router]") {
  CHECK(requires_authentication("/v1/models"));
  CHECK(requires_authentication("/v1/chat/completions"));
  CHECK(requires_authentication("/v1/models?x=1"));

  CHECK_FALSE(requires_authentication("/healthz"));
  CHECK_FALSE(requires_authentication("/healthz?x=1"));
  CHECK_FALSE(requires_authentication("/"));
  CHECK_FALSE(requires_authentication("/v1/model"));
  CHECK_FALSE(requires_authentication("/nope"));
}

TEST_CASE("The query string is not part of the route", "[server][router]") {
  CHECK(match_route("GET", "/v1/models?limit=10").m_kind == route_kind::models);
  CHECK(match_route("GET", "/healthz?verbose=1").m_kind == route_kind::health);
  CHECK(match_route("POST", "/v1/chat/completions?trace=1").m_kind == route_kind::chat);
  CHECK(match_route("POST", "/v1/models?x=1").m_kind == route_kind::wrong_method);
  CHECK(path_of("/v1/models?a=b") == "/v1/models");
  CHECK(path_of("/v1/models") == "/v1/models");
  CHECK(path_of("?only=query").empty());
}

TEST_CASE("The documented 404 and 405 bodies are stable", "[server][router]") {
  auto const missing = qoderz::server::not_found_error();
  CHECK(qoderz::server::http_status_for(missing) == 404);
  CHECK(missing.m_message == "unknown endpoint; use /v1/models or /v1/chat/completions");
  CHECK(qoderz::server::type_for(missing) == "invalid_request_error");

  auto const wrong = qoderz::server::wrong_method_error("GET");
  CHECK(qoderz::server::http_status_for(wrong) == 405);
  CHECK(wrong.m_message.find("GET") != std::string::npos);
}