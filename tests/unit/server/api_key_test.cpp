#include "server/api_key.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

using qoderz::server::authorize;

constexpr std::string_view key = "test-api-key-not-real";

std::optional<std::string> configured() {
  return std::string{key};
}

} // namespace

TEST_CASE("A server without an API key accepts every request", "[server][api_key]") {
  CHECK(authorize(std::nullopt, ""));
  CHECK(authorize(std::nullopt, "Bearer anything"));
  CHECK(authorize(std::nullopt, "garbage"));

  // An empty string is how the CLI spells "the operator passed a blank flag".
  CHECK(authorize(std::optional<std::string>{""}, ""));
  CHECK(authorize(std::optional<std::string>{""}, "Bearer whatever"));
}

TEST_CASE("A configured API key requires a matching bearer token", "[server][api_key]") {
  CHECK(authorize(configured(), "Bearer test-api-key-not-real"));
  CHECK_FALSE(authorize(configured(), "Bearer test-api-key-not-real-but-longer"));
  CHECK_FALSE(authorize(configured(), "Bearer other-key"));
  CHECK_FALSE(authorize(configured(), "Bearer "));
}

TEST_CASE("A missing or malformed Authorization header is rejected", "[server][api_key]") {
  CHECK_FALSE(authorize(configured(), ""));
  CHECK_FALSE(authorize(configured(), "   "));
  CHECK_FALSE(authorize(configured(), "test-api-key-not-real"));
  CHECK_FALSE(authorize(configured(), "Basic dGVzdA=="));
  CHECK_FALSE(authorize(configured(), "Bearer"));
  CHECK_FALSE(authorize(configured(), "Bearerish test-api-key-not-real"));
}

TEST_CASE("The bearer scheme is case insensitive", "[server][api_key]") {
  CHECK(authorize(configured(), "bearer test-api-key-not-real"));
  CHECK(authorize(configured(), "BEARER test-api-key-not-real"));
  CHECK(authorize(configured(), "BeArEr test-api-key-not-real"));
}

TEST_CASE("Surrounding whitespace is trimmed on the header and the token", "[server][api_key]") {
  CHECK(authorize(configured(), "  Bearer test-api-key-not-real  "));
  CHECK(authorize(configured(), "Bearer   test-api-key-not-real"));
  CHECK(authorize(configured(), "\t Bearer test-api-key-not-real \t"));
  // RFC 6750 separates the scheme with a space; a tab is not a separator.
  CHECK_FALSE(authorize(configured(), "Bearer\ttest-api-key-not-real\t"));

  CHECK_FALSE(authorize(configured(), "Bearer  test-api-key-not-real extra"));
}

TEST_CASE("The comparison does not short circuit on a common prefix", "[server][api_key]") {
  // A shared prefix of the right length must not be enough, and a differing tail must fail.
  CHECK_FALSE(authorize(configured(), "Bearer test-api-key-not-reaX"));
  CHECK_FALSE(authorize(configured(), "Bearer Xest-api-key-not-real"));
  CHECK_FALSE(authorize(std::optional<std::string>{"abcdefghijklmnop"}, "Bearer abcdefghijklmno"));
  CHECK(authorize(std::optional<std::string>{"abcdefghijklmnop"}, "Bearer abcdefghijklmnop"));
}
