#include "common/json.hpp"
#include "qoder/catalog/parser.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::json;
using qoderz::qoder::catalog::is_truthy;
using qoderz::qoder::catalog::model_info;
using qoderz::qoder::catalog::parse_model;
using qoderz::qoder::catalog::read_positive_int;
using qoderz::qoder::catalog::read_text;

model_info parse(std::string_view text) {
  return parse_model(json::parse(text));
}

} // namespace

TEST_CASE("read_text takes the first non-blank string", "[qoder][catalog][parser]") {
  auto const object = json::parse(R"({"a":42,"b":"  ","c":" value ","d":"ignored"})");

  CHECK(read_text(object, {"a", "b", "c", "d"}) == "value");
  CHECK(read_text(object, {"missing"}) == "");
  CHECK(read_text(json::parse("[]"), {"c"}) == "");
}

TEST_CASE("is_truthy accepts the four truthy spellings only", "[qoder][catalog][parser]") {
  CHECK(is_truthy(json::parse(R"({"v":true})"), {"v"}));
  CHECK(is_truthy(json::parse(R"({"v":1})"), {"v"}));
  CHECK(is_truthy(json::parse(R"({"v":"1"})"), {"v"}));
  CHECK(is_truthy(json::parse(R"({"v":"TRUE"})"), {"v"}));

  CHECK_FALSE(is_truthy(json::parse(R"({"v":false})"), {"v"}));
  CHECK_FALSE(is_truthy(json::parse(R"({"v":2})"), {"v"}));
  CHECK_FALSE(is_truthy(json::parse(R"({"v":"yes"})"), {"v"}));
  CHECK_FALSE(is_truthy(json::parse(R"({"v":null})"), {"v"}));
  CHECK_FALSE(is_truthy(json::parse(R"({"other":true})"), {"v"}));
  CHECK_FALSE(is_truthy(json::parse(R"({"v":0})"), {"v"}));
}

TEST_CASE("is_truthy lets the first present field decide", "[qoder][catalog][parser]") {
  CHECK_FALSE(is_truthy(json::parse(R"({"a":false,"b":true})"), {"a", "b"}));
  CHECK(is_truthy(json::parse(R"({"b":true})"), {"a", "b"}));
}

TEST_CASE("read_positive_int rejects zero, negatives and non-integers",
          "[qoder][catalog][parser]") {
  CHECK(read_positive_int(json::parse(R"({"v":7})"), {"v"}) == 7);
  CHECK(read_positive_int(json::parse(R"({"v":0})"), {"v"}) == 0);
  CHECK(read_positive_int(json::parse(R"({"v":-3})"), {"v"}) == 0);
  CHECK(read_positive_int(json::parse(R"({"v":1.5})"), {"v"}) == 0);
  CHECK(read_positive_int(json::parse(R"({"v":"9"})"), {"v"}) == 0);
  CHECK(read_positive_int(json::parse(R"({})"), {"v"}) == 0);
}

TEST_CASE("read_positive_int falls through to the next spelling", "[qoder][catalog][parser]") {
  CHECK(read_positive_int(json::parse(R"({"b":12})"), {"a", "b"}) == 12);
}

TEST_CASE("parse_model reads the name and falls back to the key", "[qoder][catalog][parser]") {
  auto const named = parse(R"({"key":"m1","display_name":"Model One","source":"app"})");
  CHECK(named.m_key == "m1");
  CHECK(named.m_name == "Model One");
  CHECK(named.m_source == "app");

  auto const unnamed = parse(R"({"key":"m2"})");
  CHECK(unnamed.m_name == "m2");
  CHECK(unnamed.m_source == "");
}

TEST_CASE("parse_model reads windows from context_config", "[qoder][catalog][parser]") {
  auto const model = parse(R"({
    "key":"m",
    "context_config":{
      "w2":{"token_count":200000,"is_default":true},
      "w1":{"token_count":100000},
      "bad":{"token_count":0},
      "w3":{"token_count":200000}
    }
  })");

  CHECK(model.m_windows == std::vector<int>{100000, 200000});
  CHECK(model.m_default_window == 200000);
  CHECK(model.m_context == 200000);
}

TEST_CASE("parse_model falls back to available_context_windows", "[qoder][catalog][parser]") {
  auto const model = parse(R"({
    "key":"m",
    "available_context_windows":[200000,100000,200000,0,"x"],
    "default_context_window":200000
  })");

  CHECK(model.m_windows == std::vector<int>{100000, 200000});
  CHECK(model.m_default_window == 200000);
  CHECK(model.m_context == 200000);
}

TEST_CASE("parse_model uses max_input_tokens when there are no windows",
          "[qoder][catalog][parser]") {
  auto const model = parse(R"({"key":"m","max_input_tokens":32000})");

  CHECK(model.m_windows.empty());
  CHECK(model.m_context == 32000);
  CHECK(model.m_default_window == 0);
}

TEST_CASE("parse_model leaves context at zero when nothing names a window",
          "[qoder][catalog][parser]") {
  auto const model = parse(R"({"key":"m"})");

  CHECK(model.m_context == 0);
  CHECK(model.m_default_window == 0);
}

TEST_CASE("parse_model reads images from is_vl", "[qoder][catalog][parser]") {
  CHECK(parse(R"({"key":"m","is_vl":true})").m_images);
  CHECK(parse(R"({"key":"m","is_vl":"1"})").m_images);
  CHECK_FALSE(parse(R"({"key":"m"})").m_images);
  CHECK_FALSE(parse(R"({"key":"m","is_vl":false})").m_images);
}

TEST_CASE("parse_model reads always-thinking from the thinking config",
          "[qoder][catalog][parser]") {
  auto const always = parse(R"({
    "key":"m",
    "thinking_config":{"enabled":{"efforts":{"low":{"is_default":true},"max":{}}}}
  })");
  CHECK(always.m_thinks);
  CHECK(always.m_always_thinks);
  CHECK(always.m_efforts == std::vector<std::string>{"low", "max"});
  CHECK(always.m_default_effort == "low");

  auto const optional = parse(R"({
    "key":"m",
    "thinking_config":{"enabled":{"efforts":{"high":{}}},"disabled":{}}
  })");
  CHECK(optional.m_thinks);
  CHECK_FALSE(optional.m_always_thinks);

  auto const off = parse(R"({"key":"m","thinking_config":{"enabled":null,"disabled":{}}})");
  CHECK_FALSE(off.m_thinks);
  CHECK_FALSE(off.m_always_thinks);
}

TEST_CASE("parse_model reads thinking from is_reasoning without a config",
          "[qoder][catalog][parser]") {
  auto const model = parse(R"({"key":"m","is_reasoning":true})");

  CHECK(model.m_thinks);
  CHECK_FALSE(model.m_always_thinks);
  CHECK(model.m_efforts.empty());
}

TEST_CASE("parse_model sorts known efforts and appends unknown ones", "[qoder][catalog][parser]") {
  auto const model = parse(R"({
    "key":"m",
    "reasoning_efforts":["max","custom","low","High",{"effort":"medium","is_default":true}]
  })");

  // Ordering is case-insensitive, but the spelling the site used is what survives.
  CHECK(model.m_efforts == std::vector<std::string>{"low", "medium", "High", "max", "custom"});
  CHECK(model.m_default_effort == "medium");
}

TEST_CASE("parse_model reads reasoning efforts from bare strings", "[qoder][catalog][parser]") {
  auto const model = parse(R"({"key":"m","reasoning_efforts":["high","low",""]})");

  CHECK(model.m_efforts == std::vector<std::string>{"low", "high"});
  CHECK(model.m_default_effort == "");
}

TEST_CASE("parse_model reads pricing fields in both spellings", "[qoder][catalog][parser]") {
  auto const camel = parse(R"({"key":"m","priceFactor":2.5,"originalPriceFactor":5})");
  CHECK(camel.m_rate == Catch::Approx(2.5));
  CHECK(camel.m_rate_was == Catch::Approx(5));
  CHECK_FALSE(camel.m_free);

  auto const snake = parse(R"({"key":"m","price_factor":3,"original_price_factor":9})");
  CHECK(snake.m_rate == Catch::Approx(3));
  CHECK(snake.m_rate_was == Catch::Approx(9));
}

TEST_CASE("parse_model marks a zero price as free", "[qoder][catalog][parser]") {
  CHECK(parse(R"({"key":"m","price_factor":0})").m_free);
  CHECK_FALSE(parse(R"({"key":"m","price_factor":1})").m_free);
  CHECK(parse(R"({"key":"m","is_free":true})").m_free);
  CHECK_FALSE(parse(R"({"key":"m"})").m_free);
}

TEST_CASE("parse_model reads a live promotion", "[qoder][catalog][parser]") {
  auto const model = parse(R"({
    "key":"m",
    "price_factor":0,
    "promotion":{"before_promotion_price_factor":4,"discount_factor":0.5}
  })");

  CHECK(model.m_rate == Catch::Approx(2));
  CHECK(model.m_rate_was == Catch::Approx(4));
  CHECK_FALSE(model.m_free);
}

TEST_CASE("parse_model ignores a switched-off promotion", "[qoder][catalog][parser]") {
  auto const priced = parse(R"({
    "key":"m",
    "price_factor":0,
    "promotion":{"active":false,"before_promotion_price_factor":4,"discount_factor":0.5}
  })");
  CHECK(priced.m_rate == Catch::Approx(0));
  CHECK(priced.m_rate_was == Catch::Approx(0));
  CHECK(priced.m_free);

  // Without a price field the free flag comes from `is_free`, not from the dead promotion.
  auto const unpriced = parse(R"({
    "key":"m",
    "promotion":{"enabled":false,"before_promotion_price_factor":4}
  })");
  CHECK(unpriced.m_rate == Catch::Approx(0));
  CHECK(unpriced.m_rate_was == Catch::Approx(0));
  CHECK_FALSE(unpriced.m_free);
}

TEST_CASE("parse_model clears rate_was when it is not above rate", "[qoder][catalog][parser]") {
  CHECK(parse(R"({"key":"m","price_factor":5,"original_price_factor":5})").m_rate_was ==
        Catch::Approx(0));
  CHECK(parse(R"({"key":"m","price_factor":5,"original_price_factor":1})").m_rate_was ==
        Catch::Approx(0));
  CHECK(parse(R"({"key":"m","price_factor":5,"original_price_factor":6})").m_rate_was ==
        Catch::Approx(6));
}

TEST_CASE("parse_model keeps the raw object as compact JSON", "[qoder][catalog][parser]") {
  auto const model = parse(R"({"key":"m","display_name":"M","nested":{"a":1}})");

  auto const raw = json::parse(model.m_raw_json);
  CHECK(raw["key"] == "m");
  CHECK(raw["nested"]["a"] == 1);
  CHECK(model.m_raw_json.find('\n') == std::string::npos);
}

TEST_CASE("parse_model returns an empty descriptor for a non-object", "[qoder][catalog][parser]") {
  auto const model = parse_model(json::parse("42"));

  CHECK(model.m_key.empty());
  CHECK(model.m_context == 0);
  CHECK(model.m_raw_json.empty());
}
