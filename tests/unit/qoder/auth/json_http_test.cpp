#include "common/json.hpp"
#include "qoder/auth/support/json_http.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace {

namespace auth = qoderz::qoder::auth;
using qoderz::json;

constexpr std::int64_t test_now = 1'760'000'000'000;
constexpr std::int64_t one_day_ms = 24 * 60 * 60 * 1000;
constexpr std::int64_t max_int64 = (std::numeric_limits<std::int64_t>::max)();

json body_of(std::string_view text) {
  return json::parse(text.begin(), text.end(), nullptr, false);
}

} // namespace

TEST_CASE("a digit string that overflows int64 is not read as an expiry",
          "[qoder][auth][json_http]") {
  auto const too_long = body_of(R"({"expires_in":"99999999999999999999999"})");
  CHECK(auth::read_expires_at(too_long, test_now, std::chrono::milliseconds{1}) ==
        test_now + one_day_ms);
  CHECK(auth::read_expires_at(too_long, test_now, std::chrono::milliseconds{1000}) ==
        test_now + one_day_ms);

  auto const just_over = body_of(R"({"expires_in":"9223372036854775808"})");
  CHECK(auth::read_expires_at(just_over, test_now, std::chrono::milliseconds{1}) ==
        test_now + one_day_ms);
}

TEST_CASE("the largest int64 expiry cannot wrap the product or the sum",
          "[qoder][auth][json_http]") {
  auto const int64_max = body_of(R"({"expires_in":"9223372036854775807"})");

  CHECK(auth::read_expires_at(int64_max, test_now, std::chrono::milliseconds{1000}) ==
        test_now + one_day_ms);
  CHECK(auth::read_expires_at(int64_max, test_now, std::chrono::milliseconds{1}) ==
        test_now + one_day_ms);

  for (auto const unit : {std::chrono::milliseconds{1}, std::chrono::milliseconds{1000}}) {
    auto const expires_at = auth::read_expires_at(int64_max, test_now, unit);
    INFO("expires_at " << expires_at << " for unit " << unit.count());
    CHECK(expires_at > test_now);
    CHECK(expires_at < max_int64 / 2);
  }
}

TEST_CASE("an expiry that overflows only when added to now falls back",
          "[qoder][auth][json_http]") {
  auto const near_limit = body_of(R"({"expires_in":"9223372036854775"})");

  auto const expires_at =
      auth::read_expires_at(near_limit, test_now, std::chrono::milliseconds{1000});
  CHECK(expires_at == test_now + one_day_ms);
  CHECK(expires_at > test_now);
}

TEST_CASE("an expiry that fits is still read exactly", "[qoder][auth][json_http]") {
  CHECK(auth::read_expires_at(body_of(R"({"expires_in":3600000})"), test_now,
                              std::chrono::milliseconds{1}) == test_now + 3'600'000);
  CHECK(auth::read_expires_at(body_of(R"({"expires_in":"7200"})"), test_now,
                              std::chrono::milliseconds{1000}) == test_now + 7'200'000);
  CHECK(auth::read_expires_at(body_of(R"({"expires_at":1893456000000})"), test_now,
                              std::chrono::milliseconds{1}) == 1'893'456'000'000);
  CHECK(auth::read_expires_at(body_of(R"({"expires_in":-5})"), test_now,
                              std::chrono::milliseconds{1}) == test_now + one_day_ms);
}

TEST_CASE("read_int keeps the fallback for anything it cannot represent",
          "[qoder][auth][json_http]") {
  CHECK(auth::read_int(body_of(R"({"expires_in":"9223372036854775807"})"), {"expires_in"}, 0) ==
        max_int64);
  CHECK(auth::read_int(body_of(R"({"expires_in":"9223372036854775808"})"), {"expires_in"}, 7) == 7);
  CHECK(auth::read_int(body_of(R"({"expires_in":"99999999999999999999999"})"), {"expires_in"}, 7) ==
        7);
  CHECK(auth::read_int(body_of(R"({"expires_in":"-1"})"), {"expires_in"}, 7) == 7);
  CHECK(auth::read_int(body_of(R"({"expires_in":"12x"})"), {"expires_in"}, 7) == 7);
  CHECK(auth::read_int(body_of(R"({"expires_in":7200})"), {"expires_in"}, 7) == 7200);
}
