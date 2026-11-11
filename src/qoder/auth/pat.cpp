#include "qoder/auth/pat.hpp"

#include "common/log.hpp"
#include "common/strings.hpp"
#include "qoder/auth/support/json_http.hpp"
#include "qoder/auth/types.hpp"
#include "qoder/errors.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::qoder::auth {
namespace {

constexpr std::string_view pat_prefix = "pt-";
constexpr std::string_view exchange_path = "/api/v1/jobToken/exchange";
constexpr std::string_view userinfo_path = "/api/v1/userinfo";
constexpr std::string_view missing_pat_message = "A Qoder personal access token is required.";
constexpr std::string_view bad_prefix_message =
    "A Qoder personal access token must start with 'pt-'.";
constexpr std::string_view refused_message = "Qoder refused the PAT — check it is still valid";
constexpr std::string_view no_token_message = "PAT exchange: Qoder returned no token";
constexpr std::string_view no_uid_message = "Qoder returned no account id for the PAT";
constexpr std::chrono::milliseconds job_expires_unit{1};

} // namespace

result<std::string> validate_pat(std::string_view pat) {
  auto const trimmed = strings::trim(pat);
  if (trimmed.empty()) {
    return std::unexpected(configuration_error(std::string{missing_pat_message}));
  }
  if (!strings::starts_with(trimmed, pat_prefix)) {
    return std::unexpected(configuration_error(std::string{bad_prefix_message}));
  }
  return std::string{trimmed};
}

boost::asio::awaitable<result<config::account_record>>
login_with_pat(net::http_client& http, config::site_definition const& site,
               config::resolved_site const& resolved, std::string pat, clock& time) {
  auto validated = validate_pat(pat);
  if (!validated) {
    co_return std::unexpected(validated.error());
  }
  auto const token = std::move(*validated);

  json const body = {{"personal_token", token}};
  auto exchanged = co_await send_json(
      http, json_post(resolved.m_openapi_base + std::string{exchange_path}, body));
  if (!exchanged) {
    co_return std::unexpected(exchanged.error());
  }

  if (exchanged->m_status == 400 || exchanged->m_status == 401 || exchanged->m_status == 403) {
    log(log_level::warn, "the PAT was refused",
        {{"site", site.m_id}, {"http_status", std::to_string(exchanged->m_status)}});
    co_return std::unexpected(make_error(error_kind::auth, std::string{refused_message}));
  }
  if (exchanged->m_status != 200) {
    co_return std::unexpected(explain(*exchanged));
  }

  auto const access = read_text(exchanged->m_body, {"token", "device_token", "access_token"});
  if (access.empty()) {
    co_return std::unexpected(make_error(error_kind::upstream, std::string{no_token_message}));
  }

  auto userinfo = co_await send_json(
      http, bearer_get(resolved.m_openapi_base + std::string{userinfo_path}, access));
  if (!userinfo) {
    co_return std::unexpected(userinfo.error());
  }
  if (userinfo->m_status != 200) {
    log(log_level::warn, "the PAT userinfo call failed",
        {{"site", site.m_id}, {"http_status", std::to_string(userinfo->m_status)}});
    // §3 of the protocol doc makes every userinfo failure an auth error, so the kind stays `auth`.
    // The wording is what was wrong: `no_uid_message` claims the account id was missing even when
    // the server answered 500 and never got that far, which reads as a broken token. Only a 200
    // with a blank uid below earns that message; everything else carries what the server actually
    // said.
    if (userinfo->m_status >= 400 && userinfo->m_status < 500) {
      co_return std::unexpected(make_error(error_kind::auth, std::string{no_uid_message}));
    }
    auto const explained = explain(*userinfo);
    co_return std::unexpected(make_error(error_kind::auth, explained.m_message, explained.m_code));
  }

  config::account_record record;
  record.m_site = resolved.m_site_id;
  record.m_vpc = resolved.m_vpc;
  record.m_access_token = access;
  record.m_refresh_token = read_text(exchanged->m_body, {"refresh_token", "refreshToken"});
  record.m_expires_at = read_expires_at(exchanged->m_body, time.now_ms(), job_expires_unit);
  record.m_uid = read_text(userinfo->m_body, {"id", "user_id", "uid"});
  record.m_display_name = read_text(userinfo->m_body, {"name", "display_name", "user_name"});
  record.m_email = read_text(userinfo->m_body, {"email"});
  if (record.m_uid.empty()) {
    co_return std::unexpected(make_error(error_kind::auth, std::string{no_uid_message}));
  }

  record.m_account_id = build_account_id(record.m_email, record.m_uid, record.m_vpc);
  record.m_machine_id = make_uuid();
  record.m_device_chat = false;
  record.m_pat_auth = true;
  record.m_pat = token;
  record.m_created_at = time.now_ms();
  log(log_level::info, "PAT sign-in succeeded", {{"site", site.m_id}, {"uid", record.m_uid}});
  co_return record;
}

} // namespace qoderz::qoder::auth
