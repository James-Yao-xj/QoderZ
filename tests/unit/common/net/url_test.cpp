#include "common/error.hpp"
#include "common/net/url.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using qoderz::error_kind;
using qoderz::net::is_loopback_host;
using qoderz::net::origin_of;
using qoderz::net::parse_url;

TEST_CASE("http URLs keep their query and fill in port 80", "[common][net][url]") {
  auto const url = parse_url("http://api.example.com/v1/models?Encode=1&x=a");
  REQUIRE(url.has_value());
  CHECK(url->m_scheme == "http");
  CHECK(url->m_host == "api.example.com");
  CHECK(url->m_port == "80");
  CHECK(url->m_target == "/v1/models?Encode=1&x=a");
  CHECK_FALSE(url->m_tls);
  CHECK(origin_of(*url) == "http://api.example.com:80");
}

TEST_CASE("https URLs default to port 443 and mark themselves as TLS", "[common][net][url]") {
  auto const url = parse_url("https://openapi.qoder.sh");
  REQUIRE(url.has_value());
  CHECK(url->m_port == "443");
  CHECK(url->m_target == "/");
  CHECK(url->m_tls);
  CHECK(origin_of(*url) == "https://openapi.qoder.sh:443");
}

TEST_CASE("an explicit port wins over the scheme default", "[common][net][url]") {
  auto const url = parse_url("http://127.0.0.1:8080/sash/api?x=1");
  REQUIRE(url.has_value());
  CHECK(url->m_port == "8080");
  CHECK(url->m_target == "/sash/api?x=1");
}

TEST_CASE("an empty path becomes a single slash", "[common][net][url]") {
  auto const bare = parse_url("http://example.com");
  REQUIRE(bare.has_value());
  CHECK(bare->m_target == "/");

  auto const with_query = parse_url("http://example.com?a=1");
  REQUIRE(with_query.has_value());
  CHECK(with_query->m_target == "/?a=1");
}

TEST_CASE("IPv6 literals lose their brackets in the host and get them back in the origin",
          "[common][net][url]") {
  auto const url = parse_url("http://[::1]:9000/health?x=1");
  REQUIRE(url.has_value());
  CHECK(url->m_host == "::1");
  CHECK(url->m_port == "9000");
  CHECK(url->m_target == "/health?x=1");
  CHECK(origin_of(*url) == "http://[::1]:9000");
}

TEST_CASE("schemes and hosts are lowercased for a stable pool key", "[common][net][url]") {
  auto const url = parse_url("HTTP://API.Example.COM/Path");
  REQUIRE(url.has_value());
  CHECK(url->m_scheme == "http");
  CHECK(url->m_host == "api.example.com");
  CHECK(url->m_target == "/Path");
  CHECK(origin_of(*url) == "http://api.example.com:80");
}

TEST_CASE("a padded port reaches the same origin as its plain form", "[common][net][url]") {
  auto const url = parse_url("http://example.com:0080/x");
  REQUIRE(url.has_value());
  CHECK(url->m_port == "80");
  CHECK(origin_of(*url) == "http://example.com:80");
}

TEST_CASE("non-http schemes are rejected", "[common][net][url][error]") {
  for (auto const* text : {"ftp://example.com/x", "ws://example.com/x", "file:///etc/passwd"}) {
    auto const url = parse_url(text);
    REQUIRE_FALSE(url.has_value());
    CHECK(url.error().m_kind == error_kind::bad_request);
    CHECK(url.error().m_message.find("http") != std::string::npos);
  }
}

TEST_CASE("a missing or empty host is rejected", "[common][net][url][error]") {
  for (auto const* text : {"http://", "http:///path", "http://:8080/x"}) {
    auto const url = parse_url(text);
    REQUIRE_FALSE(url.has_value());
    CHECK(url.error().m_kind == error_kind::bad_request);
    CHECK(url.error().m_message.find("host") != std::string::npos);
  }
}

TEST_CASE("relative references are rejected", "[common][net][url][error]") {
  for (auto const* text : {"/relative/path", "relative", "//example.com/x", ""}) {
    auto const url = parse_url(text);
    REQUIRE_FALSE(url.has_value());
    CHECK(url.error().m_kind == error_kind::bad_request);
  }
}

TEST_CASE("userinfo is rejected instead of being silently dropped", "[common][net][url][error]") {
  auto const url = parse_url("http://user:secret@example.com/x");
  REQUIRE_FALSE(url.has_value());
  CHECK(url.error().m_kind == error_kind::bad_request);
  CHECK(url.error().m_message.find("userinfo") != std::string::npos);
  CHECK(url.error().m_message.find("secret") == std::string::npos);
}

TEST_CASE("fragments are rejected instead of being silently dropped", "[common][net][url][error]") {
  for (auto const* text : {"http://example.com/x#fragment", "http://example.com/#"}) {
    auto const url = parse_url(text);
    REQUIRE_FALSE(url.has_value());
    CHECK(url.error().m_kind == error_kind::bad_request);
  }
}

TEST_CASE("a port outside the TCP range is rejected", "[common][net][url][error]") {
  for (auto const* text : {"http://example.com:65536/", "http://example.com:999999/"}) {
    auto const url = parse_url(text);
    REQUIRE_FALSE(url.has_value());
    CHECK(url.error().m_kind == error_kind::bad_request);
  }
}

TEST_CASE("the error never echoes the URL text", "[common][net][url][error]") {
  auto const url = parse_url("http://h:not-a-port/?token=very-secret");
  REQUIRE_FALSE(url.has_value());
  CHECK(url.error().m_message.find("very-secret") == std::string::npos);
}

TEST_CASE("localhost in any case counts as loopback", "[common][net][url]") {
  CHECK(is_loopback_host("localhost"));
  CHECK(is_loopback_host("LocalHost"));
  CHECK(is_loopback_host("LOCALHOST"));
  CHECK_FALSE(is_loopback_host("localhost.example.com"));
}

TEST_CASE("any 127.0.0.0/8 address counts as loopback", "[common][net][url]") {
  CHECK(is_loopback_host("127.0.0.1"));
  CHECK(is_loopback_host("127.255.255.254"));
  CHECK(is_loopback_host("127.1.2.3"));
  CHECK_FALSE(is_loopback_host("128.0.0.1"));
  CHECK_FALSE(is_loopback_host("10.0.0.1"));
  CHECK_FALSE(is_loopback_host("127.0.0.1.example.com"));
}

TEST_CASE("::1 counts as loopback with or without brackets", "[common][net][url]") {
  CHECK(is_loopback_host("::1"));
  CHECK(is_loopback_host("[::1]"));
  CHECK(is_loopback_host("0:0:0:0:0:0:0:1"));
  CHECK_FALSE(is_loopback_host("::2"));
}

TEST_CASE("non-loopback hosts are not loopback", "[common][net][url]") {
  CHECK_FALSE(is_loopback_host(""));
  CHECK_FALSE(is_loopback_host("example.com"));
  CHECK_FALSE(is_loopback_host("api3.qoder.sh"));
}