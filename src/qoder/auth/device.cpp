#include "qoder/auth/device.hpp"

#include "common/codec/base64.hpp"
#include "common/codec/hash.hpp"
#include "common/codec/random.hpp"
#include "common/log.hpp"
#include "common/strings.hpp"
#include "qoder/auth/support/browser.hpp"
#include "qoder/auth/support/json_http.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::qoder::auth {
namespace {

namespace asio = boost::asio;

constexpr std::size_t verifier_bytes = 64;
constexpr std::string_view challenge_method = "S256";
constexpr std::string_view device_path = "/device/selectAccounts";
constexpr std::string_view poll_path = "/api/v1/deviceToken/poll";
constexpr std::string_view job_token_path = "/api/v1/me/jobToken";
constexpr std::string_view userinfo_path = "/api/v1/userinfo";
constexpr std::string_view instruction_prefix = "Sign in to ";
constexpr std::string_view instruction_suffix = " in the browser and authorize this device";
constexpr std::string_view timed_out_message = "the sign-in timed out; start again";
constexpr std::string_view vpc_no_uid_message = "Qoder returned no account id for the VPC sign-in";
constexpr std::string_view no_job_token_message =
    "Qoder returned no job token for the device sign-in";
constexpr std::string_view refused_prefix = "Qoder refused the device sign-in (HTTP ";
constexpr std::string_view browser_launch_failed = "browser launch failed";
constexpr int rate_limit_status = 429;
constexpr std::chrono::milliseconds job_expires_unit{1};
constexpr std::chrono::milliseconds device_expires_unit{1000};

struct device_tokens {
  std::string m_token;
  std::string m_refresh;
  std::string m_uid;
  std::string m_user_name;
  json m_body;
};

std::string percent_encode(std::string_view value) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (auto const character : value) {
    auto const byte = static_cast<unsigned char>(character);
    bool const unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved) {
      encoded.push_back(character);
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(digits[byte >> 4U]);
    encoded.push_back(digits[static_cast<std::size_t>(byte & 0x0FU)]);
  }
  return encoded;
}

std::vector<std::byte> hex_to_bytes(std::string_view hex) {
  auto const nibble = [](char value) -> std::byte {
    auto const digit = static_cast<unsigned char>(value);
    if (digit >= '0' && digit <= '9') {
      return static_cast<std::byte>(digit - '0');
    }
    return static_cast<std::byte>(digit - 'a' + 10);
  };
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    auto const high = std::to_integer<unsigned char>(nibble(hex[index]));
    auto const low = std::to_integer<unsigned char>(nibble(hex[index + 1]));
    bytes.push_back(static_cast<std::byte>((high << 4U) | low));
  }
  return bytes;
}

std::string build_url(config::site_definition const& site, config::resolved_site const& resolved,
                      device_challenge const& challenge) {
  std::string url{resolved.m_web_base};
  url.append(device_path);
  url += "?challenge=";
  url += percent_encode(challenge.m_challenge);
  url += "&challenge_method=";
  url += challenge_method;
  url += "&nonce=";
  url += percent_encode(challenge.m_nonce);
  url += "&machine_id=";
  url += percent_encode(challenge.m_machine_id);
  url += "&client_id=";
  url += percent_encode(site.m_client_id);
  return url;
}

boost::asio::awaitable<void> wait_for(std::chrono::milliseconds duration) {
  auto executor = co_await asio::this_coro::executor;
  asio::steady_timer timer{executor, duration};
  co_await timer.async_wait(asio::use_awaitable);
}

void report(progress_callback const& progress, device_challenge const& challenge,
            std::string_view site_name, std::chrono::steady_clock::time_point started) {
  if (!progress) {
    return;
  }
  std::string instructions{instruction_prefix};
  instructions.append(site_name);
  instructions.append(instruction_suffix);
  auto event = progress_event{.m_url = challenge.m_url,
                              .m_instructions = std::move(instructions),
                              .m_elapsed = std::chrono::steady_clock::now() - started};
  try {
    progress(event);
  } catch (...) {
    log(log_level::warn, "the progress callback threw; the sign-in continues",
        {{"site", site_name}});
  }
}

} // namespace

device_challenge device_flow::make_challenge() const {
  auto const verifier = codec::random_base64url(verifier_bytes);
  auto const digest = codec::sha256_hex(std::string_view{verifier});

  device_challenge challenge;
  challenge.m_verifier = verifier;
  challenge.m_challenge = codec::base64url_encode(hex_to_bytes(digest));
  challenge.m_nonce = make_uuid();
  challenge.m_machine_id = make_uuid();
  challenge.m_url = build_url(m_site, m_resolved, challenge);
  return challenge;
}

device_flow::device_flow(net::http_client& http, config::site_definition const& site,
                         config::resolved_site const& resolved, clock& time,
                         browser_launcher launcher)
    : m_http{http}, m_site{site}, m_resolved{resolved}, m_time{time},
      m_launcher{launcher ? std::move(launcher) : browser_launcher{open_browser}} {}

boost::asio::awaitable<result<config::account_record>>
device_flow::login(login_options const& options, progress_callback const& progress) {
  auto const started = std::chrono::steady_clock::now();
  auto const challenge = make_challenge();
  report(progress, challenge, m_site.m_name, started);

  if (options.m_open_browser) {
    if (auto const opened = m_launcher(challenge.m_url); !opened) {
      // The launcher's message is free text and may embed the URL (challenge, nonce, machine id),
      // so the log carries a fixed reason and never the launcher's wording.
      log(log_level::warn, "could not open a browser for the device link",
          {{"site", m_site.m_id}, {"reason", browser_launch_failed}});
    }
  }

  auto const poll_url = m_resolved.m_openapi_base + std::string{poll_path} +
                        "?nonce=" + percent_encode(challenge.m_nonce) +
                        "&verifier=" + percent_encode(challenge.m_verifier) +
                        "&challenge_method=" + std::string{challenge_method};

  std::optional<device_tokens> tokens;
  while (!tokens.has_value()) {
    auto const sent = co_await send_json(m_http, bearer_get(poll_url, {}));
    if (!sent) {
      co_return std::unexpected(sent.error());
    }
    if (sent->m_status == 200) {
      auto token = read_text(sent->m_body, {"token", "device_token"});
      if (!token.empty()) {
        tokens = device_tokens{
            .m_token = std::move(token),
            .m_refresh = read_text(sent->m_body, {"refresh_token", "refreshToken"}),
            .m_uid = read_text(sent->m_body, {"user_id", "uid", "id"}),
            .m_user_name = read_text(sent->m_body, {"user_name", "name"}),
            .m_body = sent->m_body,
        };
        break;
      }
    }

    // The wait that follows a failed poll is what the budget has to cover, so the check is against
    // the next wake-up rather than against the moment the response arrived.
    if (std::chrono::steady_clock::now() - started + options.m_poll_interval > options.m_timeout) {
      co_return std::unexpected(make_error(error_kind::auth, std::string{timed_out_message}));
    }
    co_await wait_for(options.m_poll_interval);
    report(progress, challenge, m_site.m_name, started);
  }

  json const exchange_body = {{"clientId", std::string{m_site.m_client_id}}};
  auto exchange_request =
      json_post(m_resolved.m_openapi_base + std::string{job_token_path}, exchange_body);
  set_bearer(exchange_request, tokens->m_token);
  auto exchanged = co_await send_json(m_http, std::move(exchange_request));
  if (!exchanged) {
    co_return std::unexpected(exchanged.error());
  }

  config::account_record record;
  record.m_site = m_resolved.m_site_id;
  record.m_vpc = m_resolved.m_vpc;
  record.m_name = std::string{strings::trim(options.m_requested_name)};
  record.m_machine_id = challenge.m_machine_id;
  record.m_device_token = tokens->m_token;
  record.m_device_refresh_token = tokens->m_refresh;
  record.m_created_at = m_time.now_ms();
  // A 4xx on this endpoint means two different things depending on the site, and the branch is
  // decided by the status alone: the body's wording must not be able to move a refusal into
  // `quota`.
  bool const refused = exchanged->m_status >= 400 && exchanged->m_status < 500;
  bool const rate_limited = exchanged->m_status == rate_limit_status;

  if (exchanged->m_status == 200) {
    auto const access = read_text(exchanged->m_body, {"token", "access_token"});
    if (access.empty()) {
      co_return std::unexpected(make_error(error_kind::auth, std::string{no_job_token_message}));
    }
    record.m_access_token = access;
    record.m_refresh_token = read_text(exchanged->m_body, {"refresh_token", "refreshToken"});
    record.m_expires_at = read_expires_at(exchanged->m_body, m_time.now_ms(), job_expires_unit);
  } else if (m_site.m_device_chat && refused && !rate_limited) {
    // CN only: the status says this account may not hold a job token, so the device token pair from
    // the poll stands in for it. A 429 is not that verdict — a throttle clears on its own, and
    // stamping `device_chat` onto the record would turn a temporary limit into a permanent
    // downgrade.
    record.m_access_token = tokens->m_token;
    record.m_refresh_token = tokens->m_refresh;
    record.m_expires_at = read_expires_at(tokens->m_body, m_time.now_ms(), device_expires_unit);
    record.m_device_chat = true;
  } else if (refused && !m_site.m_device_chat) {
    // The international site has no fallback, so every 4xx — 409 and 429 included — is the upstream
    // refusing this sign-in itself, and starting over is the only thing the user can act on.
    log(log_level::warn, "device sign-in exchange failed",
        {{"site", m_site.m_id}, {"http_status", std::to_string(exchanged->m_status)}});
    auto const explained = explain(*exchanged);
    auto message = std::string{refused_prefix} + std::to_string(exchanged->m_status) +
                   "): " + explained.m_message;
    co_return std::unexpected(make_error(error_kind::auth, std::move(message), explained.m_code));
  } else {
    log(log_level::warn, "device sign-in exchange failed",
        {{"site", m_site.m_id}, {"http_status", std::to_string(exchanged->m_status)}});
    auto failure = explain(*exchanged);
    if (failure.m_kind == error_kind::upstream) {
      failure.m_message = std::string{refused_prefix} + std::to_string(exchanged->m_status) +
                          "): " + failure.m_message;
    }
    co_return std::unexpected(std::move(failure));
  }

  auto userinfo = co_await send_json(
      m_http, bearer_get(m_resolved.m_openapi_base + std::string{userinfo_path}, tokens->m_token));
  if (!userinfo) {
    co_return std::unexpected(userinfo.error());
  }
  json const nothing;
  json const& profile = userinfo->m_status == 200 ? userinfo->m_body : nothing;
  record.m_email = read_text(profile, {"email"});
  record.m_uid = read_text(profile, {"id", "user_id", "uid"});
  if (record.m_uid.empty()) {
    record.m_uid = tokens->m_uid;
  }
  record.m_display_name = read_text(profile, {"name", "display_name", "user_name"});
  if (record.m_display_name.empty()) {
    record.m_display_name = tokens->m_user_name;
  }

  if (!record.m_vpc.empty() && record.m_uid.empty()) {
    co_return std::unexpected(make_error(error_kind::auth, std::string{vpc_no_uid_message}));
  }

  record.m_account_id = build_account_id(record.m_email, record.m_uid, record.m_vpc);
  log(log_level::info, "device sign-in succeeded", {{"site", m_site.m_id}, {"uid", record.m_uid}});
  co_return record;
}

} // namespace qoderz::qoder::auth
