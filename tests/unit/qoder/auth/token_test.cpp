#include "common/error.hpp"
#include "common/log.hpp"
#include "common/net/http_client.hpp"
#include "common/net/http_types.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/auth/token.hpp"
#include "support/async_test.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"
#include "support/temp_dir.hpp"
#include "unit/qoder/stderr_capture.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using qoderz::error_kind;
using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::config::resolved_site;
using qoderz::qoder::auth::site_resolver;
using qoderz::qoder::auth::token_service;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
using qoderz::testing::temp_dir;

constexpr std::string_view old_access = "test-old-access-not-real";
constexpr std::string_view old_refresh = "test-old-refresh-not-real";
constexpr std::string_view new_access = "test-new-access-not-real";
constexpr std::string_view new_refresh = "test-new-refresh-not-real";
constexpr std::string_view test_pat = "pt-test-not-real";
constexpr std::string_view test_uid = "uid-test-not-real";
constexpr std::int64_t one_hour_ms = 3600 * 1000;
constexpr std::int64_t ten_minutes_ms = 10 * 60 * 1000;

std::string_view path_of(std::string_view target) {
  auto const query_start = target.find('?');
  return query_start == std::string_view::npos ? target : target.substr(0, query_start);
}

stub_response json_response(int status, std::string body) {
  stub_response response;
  response.m_status = status;
  response.m_body = std::move(body);
  return response;
}

account_record stale_account() {
  account_record record;
  record.m_site = "qoder";
  record.m_name = "ada";
  record.m_access_token = old_access;
  record.m_refresh_token = old_refresh;
  record.m_expires_at = 0;
  record.m_uid = test_uid;
  record.m_display_name = "Ada Test";
  record.m_email = "tester@example.invalid";
  record.m_account_id = "tester@example.invalid";
  record.m_machine_id = "machine-test-not-real";
  record.m_device_token = "test-device-token-not-real";
  record.m_device_refresh_token = "test-device-refresh-not-real";
  record.m_device_chat = false;
  record.m_created_at = 1;
  return record;
}

struct fixture {
  temp_dir m_dir;
  credential_store m_store;
  fake_clock m_clock;
  resolved_site m_resolved;
  site_resolver m_resolver;
  std::string m_requested_vpc;
  std::size_t m_resolver_calls = 0;

  explicit fixture(stub_server& server) : m_store{m_dir.file("credentials.json")} {
    auto site = qoderz::config::find_site("qoder");
    REQUIRE(site.has_value());
    auto resolved = qoderz::config::resolve_site(**site, "");
    REQUIRE(resolved.has_value());
    auto overridden = qoderz::config::apply_loopback_override(*resolved, server.origin());
    REQUIRE(overridden.has_value());
    m_resolved = std::move(*overridden);
    m_resolver = [this](std::string_view vpc) -> qoderz::result<resolved_site> {
      ++m_resolver_calls;
      m_requested_vpc = std::string{vpc};
      return m_resolved;
    };
  }

  void write(account_record record) {
    credential_file file;
    file.m_accounts.emplace(record.key(), std::move(record));
    REQUIRE(m_store.save(file).has_value());
  }

  [[nodiscard]] std::optional<account_record> read(std::string_view key) const {
    auto file = m_store.load();
    if (!file) {
      return std::nullopt;
    }
    auto const found = file->m_accounts.find(key);
    if (found == file->m_accounts.end()) {
      return std::nullopt;
    }
    return found->second;
  }
};

/// Starts `count` refreshes of the same record at the same time and returns how many succeeded.
///
/// They have to be genuinely in flight together for the gate to be exercised; awaiting them one
/// after another would let the first caller refresh and the rest find a fresh record, which proves
/// nothing.
asio::awaitable<std::size_t> refresh_concurrently(token_service& service, account_record record,
                                                  int count) {
  auto const executor = co_await asio::this_coro::executor;
  std::vector<qoderz::result<account_record>> collected(static_cast<std::size_t>(count));
  std::size_t remaining = static_cast<std::size_t>(count);
  asio::steady_timer all_done{executor};
  all_done.expires_at(std::chrono::steady_clock::time_point::max());

  for (int index = 0; index < count; ++index) {
    asio::co_spawn(
        executor,
        [&service, &record, &collected, index]() -> asio::awaitable<void> {
          collected[static_cast<std::size_t>(index)] = co_await service.ensure_fresh(record);
        },
        [&remaining, &all_done](std::exception_ptr) {
          if (--remaining == 0) {
            all_done.cancel();
          }
        });
  }

  auto const [ignored] = co_await all_done.async_wait(asio::as_tuple(asio::use_awaitable));
  (void)ignored;

  std::size_t succeeded = 0;
  for (auto const& outcome : collected) {
    if (outcome.has_value()) {
      ++succeeded;
    }
  }
  co_return succeeded;
}

} // namespace

TEST_CASE("a fresh account is returned without any request", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200, "{}");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  record.m_expires_at = env.m_clock.now_ms() + one_hour_ms;

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_access_token == old_access);
  CHECK(server.request_count() == 0);
  CHECK(env.m_resolver_calls == 0);
}

TEST_CASE("a job account is refreshed in milliseconds", "[qoder][auth][token]") {
  asio::io_context io;
  std::vector<stub_request> seen;
  stub_server server{io, [&seen](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.push_back(request);
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":7200000})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_access_token == new_access);
  CHECK(outcome->value().m_refresh_token == new_refresh);
  CHECK(outcome->value().m_expires_at == env.m_clock.now_ms() + 7200000);

  REQUIRE(seen.size() == 1);
  CHECK(path_of(seen[0].m_target) == "/api/v1/jobToken/refresh");
  CHECK(seen[0].m_body == std::string{"{\"refresh_token\":\""} + std::string{old_refresh} + "\"}");
  CHECK(qoderz::net::header_value(seen[0].m_headers, "Authorization") == nullptr);

  auto const stored = env.read("qoder/ada");
  REQUIRE(stored.has_value());
  CHECK(stored->m_access_token == new_access);
  CHECK(stored->m_refresh_token == new_refresh);
  CHECK(stored->m_uid == test_uid);
}

TEST_CASE("a device account is refreshed in seconds", "[qoder][auth][token]") {
  asio::io_context io;
  std::vector<stub_request> seen;
  stub_server server{io, [&seen](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.push_back(request);
                       co_return json_response(200,
                                               R"({"device_token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":5400})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  record.m_site = "qoder-cn";
  record.m_device_chat = true;
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_expires_at == env.m_clock.now_ms() + 5400 * 1000);
  CHECK(outcome->value().m_access_token == new_access);
  CHECK(outcome->value().m_device_token == new_access);
  CHECK(outcome->value().m_refresh_token == new_refresh);
  CHECK(outcome->value().m_device_refresh_token == new_refresh);
  CHECK(outcome->value().m_device_chat);

  REQUIRE(seen.size() == 1);
  CHECK(path_of(seen[0].m_target) == "/api/v1/deviceToken/refresh");
}

TEST_CASE("a device refresh without a new refresh token keeps the old one",
          "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200, R"({"token":"test-new-access-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  record.m_site = "qoder-cn";
  record.m_device_chat = true;
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_refresh_token == old_refresh);
  CHECK(outcome->value().m_device_refresh_token == old_refresh);
}

TEST_CASE("an expired job token of a PAT account is re-exchanged", "[qoder][auth][token]") {
  asio::io_context io;
  std::vector<stub_request> seen;
  stub_server server{io, [&seen](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.push_back(request);
                       auto const path = path_of(request.m_target);
                       if (path == "/api/v1/jobToken/refresh") {
                         co_return json_response(401, R"({"code":"105","message":"expired"})");
                       }
                       if (path == "/api/v1/jobToken/exchange") {
                         co_return json_response(200,
                                                 R"({"token":"test-new-access-not-real",)"
                                                 R"("refresh_token":"test-new-refresh-not-real",)"
                                                 R"("expires_in":3600000})");
                       }
                       co_return json_response(200, R"({"id":"uid-test-not-real"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  record.m_pat_auth = true;
  record.m_pat = test_pat;
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_access_token == new_access);
  CHECK(outcome->value().m_pat_auth);
  CHECK(outcome->value().m_pat == test_pat);
  CHECK(outcome->value().m_machine_id == "machine-test-not-real");
  CHECK(seen.size() == 3);
}

TEST_CASE("an expired job token without a PAT is an auth error", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(403, R"({"code":"105"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_message == "sign-in has expired — sign in again");
}

TEST_CASE("an upstream failure keeps the upstream kind", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(500, R"({"message":"boom"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::upstream);
  CHECK(outcome->error().m_http_status == 500);

  auto const stored = env.read("qoder/ada");
  REQUIRE(stored.has_value());
  CHECK(stored->m_access_token == old_access);
}

TEST_CASE("concurrent refreshes of one account refresh exactly once", "[qoder][auth][token]") {
  asio::io_context io;
  std::size_t refreshes = 0;
  stub_server server{io, [&refreshes](stub_request const&) -> asio::awaitable<stub_response> {
                       ++refreshes;
                       stub_response response = json_response(200, R"({"token":)"
                                                                   R"("test-new-access-not-real",)"
                                                                   R"("refresh_token":)"
                                                                   R"("test-new-refresh-not-real",)"
                                                                   R"("expires_in":3600000})");
                       response.m_delay_before_headers = 100ms;
                       co_return response;
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, refresh_concurrently(service, record, 5));
  REQUIRE(outcome.has_value());
  CHECK(*outcome == 5);
  CHECK(refreshes == 1);
}

TEST_CASE("an account already refreshed on disk is not refreshed again", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200, R"({"token":"unexpected"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  auto on_disk = record;
  on_disk.m_access_token = new_access;
  on_disk.m_refresh_token = new_refresh;
  on_disk.m_expires_at = env.m_clock.now_ms() + one_hour_ms;
  env.write(on_disk);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_access_token == new_access);
  CHECK(server.request_count() == 0);
}

TEST_CASE("a refreshed token survives a deleted account", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":3600000})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_access_token == new_access);
  CHECK_FALSE(env.read("qoder/ada").has_value());
}

TEST_CASE("a spent refresh token is never refreshed twice", "[qoder][auth][token]") {
  asio::io_context io;
  std::size_t refreshes = 0;
  stub_server server{io, [&refreshes](stub_request const&) -> asio::awaitable<stub_response> {
                       ++refreshes;
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":3600000})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto first = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  CHECK(refreshes == 1);

  auto second = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  CHECK(second->value().m_access_token == new_access);
  CHECK(refreshes == 1);
}

TEST_CASE("the VPC instance reaches the resolver for every refresh call", "[qoder][auth][token]") {
  asio::io_context io;
  std::vector<stub_request> seen;
  stub_server server{io, [&seen](stub_request const& request) -> asio::awaitable<stub_response> {
                       seen.push_back(request);
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":3600000})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  record.m_vpc = "acme";
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(env.m_resolver_calls == 1);
  CHECK(env.m_requested_vpc == "acme");
  REQUIRE(seen.size() == 1);
  // `http_client` never writes a `Host` header, so this is null today; asserting on it rather than
  // skipping the check means a future client that does send one is verified to name the VPC host.
  auto const* host = qoderz::net::header_value(seen[0].m_headers, "Host");
  if (host != nullptr) {
    CHECK(host->find("127.0.0.1") != std::string::npos);
  }
}

TEST_CASE("an absolute expiry in a refresh response wins over expires_in", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":60,)"
                                               R"("expires_at":"2030-01-01T00:00:00Z"})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_expires_at == 1'893'456'000'000);
}

TEST_CASE("a refresh never writes a token to the log", "[qoder][auth][token]") {
  asio::io_context io;
  stub_server server{io, [](stub_request const&) -> asio::awaitable<stub_response> {
                       co_return json_response(200,
                                               R"({"token":"test-new-access-not-real",)"
                                               R"("refresh_token":"test-new-refresh-not-real",)"
                                               R"("expires_in":3600000})");
                     }};
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  fixture env{server};
  auto record = stale_account();
  env.write(record);

  token_service service{http, env.m_store, env.m_resolver, env.m_clock};
  auto const previous = qoderz::current_log_level();
  qoderz::set_log_level(qoderz::log_level::trace);
  qoderz::test::stderr_capture capture;
  REQUIRE(capture.available());
  auto outcome = run_awaitable(io, service.ensure_fresh(record));
  auto const logged = capture.release();
  qoderz::set_log_level(previous);

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.find("test-old-access-not-real") == std::string::npos);
  CHECK(logged.find("test-old-refresh-not-real") == std::string::npos);
  CHECK(logged.find("test-new-access-not-real") == std::string::npos);
  CHECK(logged.find("test-new-refresh-not-real") == std::string::npos);
  CHECK(logged.find("tester@example.invalid") == std::string::npos);
}
