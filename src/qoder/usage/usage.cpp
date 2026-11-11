#include "qoder/usage/usage.hpp"

#include "common/json.hpp"
#include "common/log.hpp"
#include "common/net/http_types.hpp"
#include "common/strings.hpp"
#include "qoder/auth/support/json_http.hpp"
#include "qoder/errors.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::qoder::usage {
namespace {

constexpr std::string_view usage_path = "/sash/api/v2/me/usage";
constexpr std::string_view missing_quota_message = "Qoder: missing quota data";
constexpr std::string_view enterprise_plan = "Enterprise";
constexpr std::string_view default_unit = "credits";
constexpr int http_ok = 200;
constexpr int percent_limit = 100;
constexpr std::int64_t seconds_upper_bound = 100'000'000'000;
constexpr std::int64_t max_instant_ms = 253'402'300'799'999;
constexpr std::int64_t milliseconds_per_second = 1000;

/// One quota block plus the name to show when the block carries none.
struct window_slot {
  std::string_view m_fallback_name;
  std::string_view m_camel;
  std::string_view m_snake;
};

constexpr std::array<window_slot, 3> single_windows{{
    {"Credits", "userQuota", "user_quota"},
    {"Add-on credits", "addOnQuota", "add_on_quota"},
    {"Shared credits", "orgResourcePackage", "org_resource_package"},
}};

constexpr std::string_view dedicated_fallback_name = "Dedicated credits";

bool all_digits(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  return std::all_of(text.begin(), text.end(),
                     [](char value) { return value >= '0' && value <= '9'; });
}

/// A field as text: a string is trimmed, a number becomes its literal spelling, anything else is
/// empty. Numbers are read as text because the protocol then parses them as an instant either way.
std::string read_scalar_text(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return {};
  }
  auto const value = object.find(name);
  if (value == object.end()) {
    return {};
  }
  if (value->is_string()) {
    return std::string{strings::trim(value->get_ref<std::string const&>())};
  }
  if (value->is_number()) {
    return value->dump();
  }
  return {};
}

/// The first of `names` that carries text, so a camelCase field wins over its snake_case twin.
std::string first_text(json const& object, std::initializer_list<std::string_view> names) {
  for (auto const name : names) {
    auto text = read_scalar_text(object, name);
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

std::optional<json> object_member(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  auto const value = object.find(name);
  if (value == object.end() || !value->is_object()) {
    return std::nullopt;
  }
  return *value;
}

/// `expiresAt` / `expires_at` as Unix milliseconds; a number below 1e11 counts as seconds, and an
/// instant past the year 9999 is rejected rather than rendered as a nonsensical date.
std::optional<std::int64_t> read_until(json const& object) {
  auto const text = first_text(object, {"expiresAt", "expires_at"});
  if (text.empty()) {
    return std::nullopt;
  }

  if (!all_digits(text)) {
    return qoder::auth::parse_timestamp_ms(text);
  }

  std::int64_t number = 0;
  for (auto const digit : text) {
    if (number > (1'000'000'000'000'000'000LL - 9) / 10) {
      return std::nullopt;
    }
    number = number * 10 + (digit - '0');
  }
  if (number <= 0) {
    return std::nullopt;
  }

  auto const milliseconds =
      number < seconds_upper_bound ? number * milliseconds_per_second : number;
  if (milliseconds > max_instant_ms) {
    return std::nullopt;
  }
  return milliseconds;
}

/// A finite amount, or `value` untouched; JSON cannot carry NaN, but a huge literal can still
/// overflow to infinity, and an infinite window would render as `inf / inf credits`.
bool read_amount(json const& block, std::string_view name, double& value, bool& present) {
  if (!block.is_object()) {
    return false;
  }
  auto const found = block.find(name);
  if (found == block.end()) {
    return false;
  }
  present = true;
  if (!found->is_number()) {
    return false;
  }
  value = found->get<double>();
  return std::isfinite(value);
}

/// The first of `names` that is present at all; `present` is set for a non-number too, so a broken
/// amount drops the whole window instead of silently falling back to the other field.
bool read_amount(json const& block, std::initializer_list<std::string_view> names, double& value,
                 bool& present) {
  for (auto const name : names) {
    if (read_amount(block, name, value, present)) {
      return true;
    }
    if (present) {
      return false;
    }
  }
  present = false;
  return false;
}

/// `"0.##"`: up to two decimals, trailing zeros and a trailing dot removed.
std::string format_amount(double value) {
  char buffer[64];
  auto const written = std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  if (written <= 0) {
    return "0";
  }
  std::string text(buffer, static_cast<std::size_t>(written));
  auto const dot = text.find('.');
  if (dot != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text;
}

std::optional<usage_window> make_window(json const& block, std::string_view fallback_name) {
  double total = 0;
  bool total_present = false;
  if (!read_amount(block, {"total", "cap"}, total, total_present) || total <= 0) {
    return std::nullopt;
  }

  double used = 0;
  bool used_present = false;
  auto const has_used = read_amount(block, {"used"}, used, used_present);

  double remaining = 0;
  bool remaining_present = false;
  auto const has_remaining = read_amount(block, {"remaining"}, remaining, remaining_present);

  if ((used_present && !has_used) || (remaining_present && !has_remaining) ||
      (!has_used && !has_remaining)) {
    return std::nullopt;
  }
  if (!has_used) {
    used = total - remaining;
  }
  if (used < 0) {
    return std::nullopt;
  }

  auto name = first_text(block, {"name"});
  if (name.empty()) {
    name = std::string{fallback_name};
  }
  auto unit = first_text(block, {"unit"});
  if (unit.empty()) {
    unit = std::string{default_unit};
  }

  auto const ratio = std::min(static_cast<double>(percent_limit), percent_limit * used / total);
  usage_window window;
  window.m_name = std::move(name);
  window.m_unit = std::move(unit);
  window.m_used = used;
  window.m_total = total;
  window.m_percent = static_cast<int>(ratio);
  window.m_display = format_amount(used) + " / " + format_amount(total) + " " + window.m_unit;
  return window;
}

void add_single_windows(json const& usage_block, std::vector<usage_window>& windows) {
  for (auto const& slot : single_windows) {
    std::optional<json> block = object_member(usage_block, slot.m_camel);
    if (!block.has_value()) {
      block = object_member(usage_block, slot.m_snake);
    }
    if (!block.has_value()) {
      continue;
    }
    if (auto window = make_window(*block, slot.m_fallback_name); window.has_value()) {
      windows.push_back(std::move(*window));
    }
  }
}

void add_dedicated_windows(json const& usage_block, std::vector<usage_window>& windows) {
  if (!usage_block.is_object()) {
    return;
  }
  for (auto const name : {"dedicatedResourcePackages", "dedicated_resource_packages"}) {
    auto const found = usage_block.find(name);
    if (found == usage_block.end() || !found->is_array()) {
      continue;
    }
    for (auto const& block : *found) {
      if (!block.is_object()) {
        continue;
      }
      if (auto window = make_window(block, dedicated_fallback_name); window.has_value()) {
        windows.push_back(std::move(*window));
      }
    }
  }
}

std::optional<json> quota_block(json const& root) {
  for (auto const name : {"qoderUsage", "qoder_usage"}) {
    auto block = object_member(root, name);
    if (block.has_value()) {
      return block;
    }
  }
  return std::nullopt;
}

} // namespace

std::string select_token(config::account_record const& account) {
  bool const pat_only = account.m_pat_auth && account.m_device_token.empty();
  if (!pat_only && !account.m_device_token.empty()) {
    return account.m_device_token;
  }
  return account.m_access_token;
}

result<usage_snapshot> parse_usage(std::string_view body) {
  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::unexpected(upstream_error(std::string{missing_quota_message}));
  }

  auto const display_mode = first_text(parsed, {"displayMode", "display_mode"});
  if (strings::equals_ignore_case(display_mode, "enterprise")) {
    usage_snapshot snapshot;
    snapshot.m_plan = std::string{enterprise_plan};
    return snapshot;
  }

  auto const usage_block = quota_block(parsed);
  if (!strings::equals_ignore_case(display_mode, "qoder") || !usage_block.has_value()) {
    return std::unexpected(upstream_error(std::string{missing_quota_message}));
  }

  usage_snapshot snapshot;
  auto plan = first_text(parsed, {"userType", "user_type"});
  if (plan.empty()) {
    plan = first_text(*usage_block, {"userType", "user_type"});
  }
  if (!plan.empty()) {
    snapshot.m_plan = std::move(plan);
  }

  snapshot.m_until_ms = read_until(parsed);
  if (!snapshot.m_until_ms.has_value()) {
    snapshot.m_until_ms = read_until(*usage_block);
  }

  add_single_windows(*usage_block, snapshot.m_windows);
  add_dedicated_windows(*usage_block, snapshot.m_windows);
  return snapshot;
}

usage_service::usage_service(net::http_client& http, clock& time) : m_http{http}, m_time{time} {}

boost::asio::awaitable<result<usage_snapshot>>
usage_service::fetch(config::resolved_site const& site, config::account_record const& account) {
  std::string url{site.m_openapi_base};
  url.append(usage_path);

  net::http_request request;
  request.m_method = "GET";
  request.m_url = std::move(url);
  request.m_headers.push_back(net::http_header{.m_name = "Accept", .m_value = "application/json"});
  request.m_headers.push_back(net::http_header{.m_name = "Accept-Encoding", .m_value = "identity"});
  request.m_headers.push_back(net::http_header{.m_name = "Cosy-ClientType", .m_value = "10"});
  request.m_headers.push_back(net::http_header{.m_name = "User-Agent", .m_value = "Qoder"});
  request.m_headers.push_back(
      net::http_header{.m_name = "Authorization", .m_value = "Bearer " + select_token(account)});

  auto response = co_await m_http.send(request);
  if (!response) {
    co_return std::unexpected(std::move(response.error()));
  }

  if (response->m_status != http_ok) {
    log(log_level::warn, "usage request failed",
        {{"site", site.m_site_id},
         {"uid", account.m_uid},
         {"status", std::to_string(response->m_status)}});
    co_return std::unexpected(to_error(from_response(response->m_status, response->m_body)));
  }

  auto snapshot = parse_usage(response->m_body);
  if (!snapshot) {
    co_return std::unexpected(std::move(snapshot.error()));
  }

  log(log_level::debug, "usage read",
      {{"site", site.m_site_id},
       {"uid", account.m_uid},
       {"status", std::to_string(response->m_status)}});
  co_return std::move(*snapshot);
}

} // namespace qoderz::qoder::usage
