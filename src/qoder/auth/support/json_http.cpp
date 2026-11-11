#include "qoder/auth/support/json_http.hpp"

#include "common/codec/random.hpp"
#include "common/strings.hpp"
#include "qoder/errors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::qoder::auth {
namespace {

constexpr std::string_view iso_utc_suffix = "Z";
constexpr std::string_view content_type_header = "Content-Type";
constexpr std::string_view authorization_header = "Authorization";
constexpr std::string_view accept_header = "Accept";
constexpr std::string_view application_json = "application/json";
constexpr std::string_view bearer_prefix = "Bearer ";
constexpr std::int64_t milliseconds_per_second = 1000;
constexpr std::int64_t seconds_upper_bound = 1'000'000'000'000;
constexpr std::int64_t max_int64 = (std::numeric_limits<std::int64_t>::max)();

bool is_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

/// Exactly `count` digits at `offset`, or `nullopt` when the field runs past the end.
std::optional<int> read_digits(std::string_view text, std::size_t offset, std::size_t count) {
  if (offset + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (std::size_t index = offset; index < offset + count; ++index) {
    if (!is_digit(text[index])) {
      return std::nullopt;
    }
    value = value * 10 + (text[index] - '0');
  }
  return value;
}

/// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's civil-from-days),
/// which keeps leap-year handling in one place instead of a lookup table.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  auto const era = (year >= 0 ? year : year - 399) / 400;
  auto const year_of_era = static_cast<unsigned>(year - era * 400);
  auto const day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  auto const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

bool is_number(json const& value) noexcept {
  return value.is_number();
}

/// `expires_in * unit`, or `nullopt` when the product does not fit. A wrapped product would
/// silently turn a malformed field into a plausible-looking expiry instead of leaving the default.
std::optional<std::int64_t> scaled_milliseconds(std::int64_t expires_in,
                                                std::int64_t unit_ms) noexcept {
  if (expires_in < 0 || unit_ms <= 0 || expires_in > max_int64 / unit_ms) {
    return std::nullopt;
  }
  return expires_in * unit_ms;
}

/// `now_ms + delta`, or `nullopt` when the sum would leave the range a timestamp can have.
std::optional<std::int64_t> add_milliseconds(std::int64_t now_ms, std::int64_t delta_ms) noexcept {
  if (delta_ms < 0 || now_ms < 0 || delta_ms > max_int64 - now_ms) {
    return std::nullopt;
  }
  return now_ms + delta_ms;
}

} // namespace

net::http_request json_post(std::string url, json const& body) {
  net::http_request request;
  request.m_method = "POST";
  request.m_url = std::move(url);
  request.m_headers.push_back(net::http_header{.m_name = std::string{accept_header},
                                               .m_value = std::string{application_json}});
  request.m_headers.push_back(net::http_header{.m_name = std::string{content_type_header},
                                               .m_value = std::string{application_json}});
  request.m_body = body.dump();
  return request;
}

void set_bearer(net::http_request& request, std::string_view token) {
  if (token.empty()) {
    return;
  }
  std::string value{bearer_prefix};
  value.append(token);
  request.m_headers.push_back(
      net::http_header{.m_name = std::string{authorization_header}, .m_value = std::move(value)});
}

net::http_request bearer_get(std::string url, std::string_view token) {
  net::http_request request;
  request.m_method = "GET";
  request.m_url = std::move(url);
  request.m_headers.push_back(net::http_header{.m_name = std::string{accept_header},
                                               .m_value = std::string{application_json}});
  set_bearer(request, token);
  return request;
}

boost::asio::awaitable<result<json_exchange>> send_json(net::http_client& http,
                                                        net::http_request request) {
  auto outcome = co_await http.send(request);
  if (!outcome) {
    co_return std::unexpected(std::move(outcome.error()));
  }
  json_exchange exchange;
  exchange.m_status = outcome->m_status;
  exchange.m_text = std::move(outcome->m_body);
  auto const trimmed = strings::trim(exchange.m_text);
  if (!trimmed.empty()) {
    json parsed = json::parse(trimmed, nullptr, false);
    if (!parsed.is_discarded()) {
      exchange.m_body = std::move(parsed);
    }
  }
  co_return exchange;
}

error explain(json_exchange const& exchange) {
  return to_error(from_response(exchange.m_status, exchange.m_text));
}

std::string read_text(json const& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object()) {
    return {};
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value == object.end() || !value->is_string()) {
      continue;
    }
    auto const text = value->get<std::string>();
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

std::int64_t read_int(json const& object, std::initializer_list<std::string_view> names,
                      std::int64_t fallback) {
  if (!object.is_object()) {
    return fallback;
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value == object.end()) {
      continue;
    }
    if (is_number(*value)) {
      return value->get<std::int64_t>();
    }
    if (value->is_string()) {
      auto const text = strings::trim(value->get_ref<std::string const&>());
      std::int64_t parsed_value = 0;
      bool valid = !text.empty();
      for (auto const digit : text) {
        if (!is_digit(digit)) {
          valid = false;
          break;
        }
        // Checked before each multiply: a digit string long enough to overflow would otherwise wrap
        // into a small positive number, and a wrapped `expires_in` reads as a real one.
        if (parsed_value > (max_int64 - (digit - '0')) / 10) {
          valid = false;
          break;
        }
        parsed_value = parsed_value * 10 + (digit - '0');
      }
      if (valid) {
        return parsed_value;
      }
    }
  }
  return fallback;
}

std::optional<std::int64_t> parse_timestamp_ms(std::string_view text) {
  auto trimmed = strings::trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  bool all_digits = true;
  for (auto const value : trimmed) {
    if (!is_digit(value)) {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    std::int64_t number = 0;
    for (auto const value : trimmed) {
      if (number > (1'000'000'000'000'000'000LL - 9) / 10) {
        return std::nullopt;
      }
      number = number * 10 + (value - '0');
    }
    return number < seconds_upper_bound ? number * milliseconds_per_second : number;
  }

  if (trimmed.size() < 19) {
    return std::nullopt;
  }
  auto const year = read_digits(trimmed, 0, 4);
  auto const month = read_digits(trimmed, 5, 2);
  auto const day = read_digits(trimmed, 8, 2);
  auto const hour = read_digits(trimmed, 11, 2);
  auto const minute = read_digits(trimmed, 14, 2);
  auto const second = read_digits(trimmed, 17, 2);
  if (!year || !month || !day || !hour || !minute || !second) {
    return std::nullopt;
  }
  if (trimmed[4] != '-' || trimmed[7] != '-' || (trimmed[10] != 'T' && trimmed[10] != ' ') ||
      trimmed[13] != ':' || trimmed[16] != ':') {
    return std::nullopt;
  }
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
      *second > 60) {
    return std::nullopt;
  }

  auto const days =
      days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
  auto const seconds = days * 86400 + *hour * 3600 + *minute * 60 + std::min(*second, 59);
  std::int64_t offset_seconds = 0;

  auto const rest = trimmed.substr(19);
  if (!rest.empty()) {
    if (rest.front() == '.') {
      std::size_t index = 1;
      while (index < rest.size() && is_digit(rest[index])) {
        ++index;
      }
      auto const tail = rest.substr(index);
      if (!tail.empty()) {
        if (tail == iso_utc_suffix) {
          offset_seconds = 0;
        } else if (tail.size() == 6 && (tail[0] == '+' || tail[0] == '-')) {
          auto const hours = read_digits(tail, 1, 2);
          auto const minutes = read_digits(tail, 4, 2);
          if (!hours || !minutes || tail[3] != ':') {
            return std::nullopt;
          }
          offset_seconds = (*hours * 3600 + *minutes * 60) * (tail[0] == '-' ? -1 : 1);
        } else {
          return std::nullopt;
        }
      }
    } else if (rest == iso_utc_suffix) {
      offset_seconds = 0;
    } else if (rest.size() == 6 && (rest[0] == '+' || rest[0] == '-')) {
      auto const hours = read_digits(rest, 1, 2);
      auto const minutes = read_digits(rest, 4, 2);
      if (!hours || !minutes || rest[3] != ':') {
        return std::nullopt;
      }
      offset_seconds = (*hours * 3600 + *minutes * 60) * (rest[0] == '-' ? -1 : 1);
    } else {
      return std::nullopt;
    }
  }

  return (seconds - offset_seconds) * milliseconds_per_second;
}

std::int64_t read_expires_at(json const& object, std::int64_t now_ms,
                             std::chrono::milliseconds expires_in_unit,
                             std::chrono::milliseconds fallback) {
  if (object.is_object()) {
    constexpr std::array<std::string_view, 4> absolute_fields{"expires_at", "expire_time",
                                                              "expiresAt", "expireTime"};
    for (auto const name : absolute_fields) {
      auto const value = object.find(name);
      if (value == object.end()) {
        continue;
      }
      if (value->is_string()) {
        if (auto const parsed = parse_timestamp_ms(value->get_ref<std::string const&>());
            parsed.has_value()) {
          return *parsed;
        }
        continue;
      }
      if (is_number(*value)) {
        auto const number = value->get<std::int64_t>();
        return number < seconds_upper_bound ? number * milliseconds_per_second : number;
      }
    }
  }

  auto const expires_in = read_int(object, {"expires_in", "expiresIn"}, -1);
  if (expires_in >= 0) {
    // A product or a sum that does not fit leaves `expires_in` unread rather than wrapping: a
    // wrapped value is a timestamp that prints normally, sits in the past, and makes the account
    // look expired the moment it was stored.
    if (auto const scaled = scaled_milliseconds(expires_in, expires_in_unit.count());
        scaled.has_value()) {
      if (auto const deadline = add_milliseconds(now_ms, *scaled); deadline.has_value()) {
        return *deadline;
      }
    }
  }
  return add_milliseconds(now_ms, fallback.count()).value_or(now_ms);
}

std::string make_uuid() {
  auto const raw = codec::random_bytes(16);
  constexpr std::string_view hex_digits = "0123456789abcdef";
  constexpr std::string_view shape = "00000000-0000-0000-0000-000000000000";

  std::string digits;
  digits.reserve(32);
  for (std::size_t index = 0; index < raw.size(); ++index) {
    auto byte = static_cast<unsigned char>(raw[index]);
    if (index == 6) {
      byte = static_cast<unsigned char>((byte & 0x0FU) | 0x40U);
    } else if (index == 8) {
      byte = static_cast<unsigned char>((byte & 0x3FU) | 0x80U);
    }
    digits.push_back(hex_digits[byte >> 4U]);
    digits.push_back(hex_digits[byte & 0x0FU]);
  }

  std::string uuid;
  uuid.reserve(shape.size());
  std::size_t next = 0;
  for (auto const slot : shape) {
    uuid.push_back(slot == '-' ? '-' : digits[next++]);
  }
  return uuid;
}

} // namespace qoderz::qoder::auth
