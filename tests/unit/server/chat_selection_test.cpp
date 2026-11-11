#include "chat_test_support.hpp"
#include "server/error_writer.hpp"

namespace {

using namespace qoderz::test::chat;

} // namespace

TEST_CASE("an unknown model is a 400 after one forced second pass", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  auto done = ask(scope, "ghost", true, "session-a");
  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 400);
  CHECK(done.m_outcome.error().m_message == R"(unknown or disabled model "ghost")");
  CHECK(scope.listing_calls() == 4);
  CHECK(scope.chat_calls() == 0);
  CHECK_FALSE(done.sink().m_started);
}

TEST_CASE("a candidate that already failed is not retried by the second pass", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.listing_status("ada", 429);
  auto done = ask(scope, "ghost", true, "session-b");
  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(done.m_outcome.error().m_kind == error_kind::quota);
  CHECK(scope.listing_calls() == 3);
  CHECK(scope.m_health.is_cooling("qoder/ada"));
}

TEST_CASE("the last auth or quota failure wins and never leaks a token", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.listing_status("ada", 429);
  scope.listing_status("bob", 401);
  auto done = ask(scope, "ultimate", true, "session-c");
  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(done.m_outcome.error().m_kind == error_kind::auth);
  auto const body = qoderz::server::to_json(done.m_outcome.error());
  CHECK(body.find("access-") == std::string::npos);
  CHECK(body.find("refresh-") == std::string::npos);
  CHECK(body.find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("a busy upstream is answered directly instead of failing over", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  scope.chat(
      "ada", 503,
      R"({"code":"10605","queue":{"isQueued":true,"retryAfterSeconds":3},"message":"queued"})");
  auto done = ask(scope, "ultimate", true, "session-d");
  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(done.m_outcome.error().m_kind == error_kind::busy);
  CHECK(scope.chat_calls() == 1);
  CHECK_FALSE(scope.m_health.is_cooling("qoder/ada"));
}

TEST_CASE("a pool with no usable account answers before any upstream call", "[server][chat]") {
  SECTION("no account at all") {
    fixture scope{std::vector<std::string>{}};
    auto done = ask(scope, "ultimate", true);
    REQUIRE_FALSE(done.m_outcome.has_value());
    CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 401);
    CHECK(done.m_outcome.error().m_message == "not signed in; run: qoderz login");
    CHECK(scope.listing_calls() == 0);
    CHECK(scope.chat_calls() == 0);
  }

  SECTION("every account cooling on auth") {
    fixture scope{{"ada", "bob"}};
    scope.m_health.report_failure("qoder/ada", failure_kind::auth);
    scope.m_health.report_failure("qoder/bob", failure_kind::auth);
    auto done = ask(scope, "ultimate", true);
    REQUIRE_FALSE(done.m_outcome.has_value());
    CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 401);
    CHECK(scope.listing_calls() == 0);
  }

  SECTION("a quota cooldown makes the fully cooling pool a 429") {
    fixture scope{{"ada", "bob"}};
    scope.m_health.report_failure("qoder/ada", failure_kind::quota);
    scope.m_health.report_failure("qoder/bob", failure_kind::auth);
    auto done = ask(scope, "ultimate", true);
    REQUIRE_FALSE(done.m_outcome.has_value());
    CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 429);
  }
}

TEST_CASE("an unusable VPC is a configuration error before any upstream call", "[server][chat]") {
  fixture scope{{"ada"}, "invalid", false, "qoder-cn"};
  auto done = ask(scope, "ultimate", true, "session-e");
  REQUIRE_FALSE(done.m_outcome.has_value());
  CHECK(qoderz::server::http_status_for(done.m_outcome.error()) == 500);
  CHECK(done.m_outcome.error().m_kind == error_kind::configuration);
  CHECK(scope.listing_calls() == 0);
  CHECK(scope.chat_calls() == 0);
}

TEST_CASE("the request body is parsed before any account is touched", "[server][chat]") {
  fixture scope;
  auto handler = scope.make_handler();
  auto deliver = [&](std::string body) {
    recording_sink sink;
    chat_http_request request;
    request.m_body = std::move(body);
    request.m_stream = true;
    auto outcome = run_awaitable(scope.m_io, handler.handle(request, sink));
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
  };

  auto invalid = deliver("{not json");
  REQUIRE_FALSE(invalid.has_value());
  CHECK(qoderz::server::http_status_for(invalid.error()) == 400);
  CHECK(invalid.error().m_message == "the request body is not valid JSON");

  auto no_messages = deliver(R"({"model":"ultimate"})");
  REQUIRE_FALSE(no_messages.has_value());
  CHECK(no_messages.error().m_message == "messages is required");

  auto no_model = deliver(R"({"messages":[]})");
  REQUIRE_FALSE(no_model.has_value());
  CHECK(no_model.error().m_message == "model is required");

  auto blank = deliver(R"({"model":"QODER/  ","messages":[]})");
  REQUIRE_FALSE(blank.has_value());
  CHECK(blank.error().m_message == "model is required");

  CHECK(scope.listing_calls() == 0);
  CHECK(scope.chat_calls() == 0);
}

TEST_CASE("a site prefixed model is matched against the catalogue key", "[server][chat]") {
  fixture scope;
  scope.listing("ada", {"Ultimate"});
  auto done = ask(scope, "qoder/ultimate", true, "session-f");
  REQUIRE(done.m_outcome.has_value());
  REQUIRE(scope.chat_calls() == 1);
  CHECK(scope.sent_body(0).at("model_config").at("key") == "Ultimate");
}

TEST_CASE("different sessions rotate while one session stays bound", "[server][chat]") {
  fixture scope{{"ada", "bob"}};
  for (auto const name : {"one", "two", "three"}) {
    auto done = ask(scope, "ultimate", true, name);
    REQUIRE(done.m_outcome.has_value());
  }

  REQUIRE(scope.m_affinity.size() == 3);
  auto const one = scope.m_affinity.bound("one", scope.m_clock.now_ms());
  auto const two = scope.m_affinity.bound("two", scope.m_clock.now_ms());
  auto const three = scope.m_affinity.bound("three", scope.m_clock.now_ms());
  REQUIRE(one.has_value());
  REQUIRE(two.has_value());
  REQUIRE(three.has_value());
  CHECK(*one != *two);
  CHECK(*one == *three);

  auto const before = scope.chat_calls();
  auto replay = ask(scope, "ultimate", true, "two");
  REQUIRE(replay.m_outcome.has_value());
  auto const bound = scope.m_affinity.bound("two", scope.m_clock.now_ms());
  REQUIRE(bound.has_value());
  CHECK(*bound == *two);
  CHECK(scope.chat_calls() == before + 1);
}
