#include "common/strings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

using qoderz::strings::ascii_lower;
using qoderz::strings::equals_ignore_case;
using qoderz::strings::starts_with;
using qoderz::strings::trim;
using qoderz::strings::trim_end;
using qoderz::strings::trim_start;

TEST_CASE("trim strips surrounding ASCII whitespace", "[common][strings]") {
  CHECK(trim("") == std::string_view{});
  CHECK(trim(" \t\r\n\v\f ") == std::string_view{});
  CHECK(trim("  hello  ") == "hello");
  CHECK(trim("\t a b \r\n") == "a b");
  CHECK(trim("a b") == "a b");
  CHECK(trim_start(" \tx ") == "x ");
  CHECK(trim_end(" x \t") == " x");
  CHECK(trim_start("x") == "x");
}

TEST_CASE("ascii_lower folds only A-Z", "[common][strings]") {
  CHECK(ascii_lower('A') == 'a');
  CHECK(ascii_lower('Z') == 'z');
  CHECK(ascii_lower('a') == 'a');
  CHECK(ascii_lower('0') == '0');
  CHECK(ascii_lower('[') == '[');
  CHECK(ascii_lower(static_cast<char>(0xC4)) == static_cast<char>(0xC4));
}

TEST_CASE("equals_ignore_case compares ASCII case only", "[common][strings]") {
  CHECK(equals_ignore_case("INFO", "info"));
  CHECK(equals_ignore_case("", ""));
  CHECK(equals_ignore_case("MiXeD", "mixed"));
  CHECK_FALSE(equals_ignore_case("info", "infos"));
  CHECK_FALSE(equals_ignore_case("info", " information"));
  CHECK_FALSE(equals_ignore_case(" info", "info"));
  CHECK_FALSE(equals_ignore_case("ab", "ac"));
}

TEST_CASE("starts_with matches a literal prefix", "[common][strings]") {
  CHECK(starts_with("application/json", "application"));
  CHECK(starts_with("abc", ""));
  CHECK(starts_with("", ""));
  CHECK_FALSE(starts_with("", "a"));
  CHECK_FALSE(starts_with("ab", "abc"));
  CHECK_FALSE(starts_with("Application", "application"));
}
