#pragma once

#include "common/error.hpp"
#include "common/net/http_types.hpp"
#include "config/site.hpp"
#include "qoder/auth/device.hpp"
#include "qoder/auth/types.hpp"
#include "support/fake_clock.hpp"
#include "support/stub_server.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// A site pinned at the loopback stub, a recorder for what went on the wire, and the JSON shapes
/// the device endpoints answer with — everything the device cases need that is not the case itself.
namespace qoderz::test::device {
using qoderz::config::resolved_site;
using qoderz::config::site_definition;
using qoderz::qoder::auth::progress_callback;
using qoderz::qoder::auth::progress_event;
using qoderz::test::fake_clock;
using qoderz::test::stub_request;
using qoderz::test::stub_response;
using qoderz::test::stub_server;
inline constexpr std::string_view poll_token = "test-device-token-not-real";
inline constexpr std::string_view poll_refresh = "test-device-refresh-not-real";
inline constexpr std::string_view job_token = "test-job-token-not-real";
inline constexpr std::string_view job_refresh = "test-job-refresh-not-real";
inline constexpr std::string_view test_uid = "uid-test-not-real";
inline constexpr std::string_view test_email = "tester@example.invalid";

inline char hex_digit_to_char(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<char>(value - '0');
  }
  return static_cast<char>(value - 'a' + 10);
}

inline std::string percent_decode(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size()) {
      auto const high = hex_digit_to_char(text[index + 1]);
      auto const low = hex_digit_to_char(text[index + 2]);
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
      continue;
    }
    decoded.push_back(text[index]);
  }
  return decoded;
}

inline std::optional<std::string> query_value(std::string_view target, std::string_view name) {
  auto const query_start = target.find('?');
  if (query_start == std::string_view::npos) {
    return std::nullopt;
  }
  auto const query = target.substr(query_start + 1);
  std::size_t position = 0;
  while (position <= query.size()) {
    auto const next = query.find('&', position);
    auto const pair = query.substr(position, next == std::string_view::npos ? std::string_view::npos
                                                                            : next - position);
    auto const equals = pair.find('=');
    if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
      return percent_decode(pair.substr(equals + 1));
    }
    if (next == std::string_view::npos) {
      return std::nullopt;
    }
    position = next + 1;
  }
  return std::nullopt;
}

inline std::string_view path_of(std::string_view target) {
  auto const query_start = target.find('?');
  return query_start == std::string_view::npos ? target : target.substr(0, query_start);
}

inline stub_response json_response(int status, std::string body) {
  stub_response response;
  response.m_status = status;
  response.m_body = std::move(body);
  response.m_headers.push_back(
      qoderz::net::http_header{.m_name = "Content-Type", .m_value = "application/json"});
  return response;
}

/// Records every request the stub answers so a test can assert on what went on the wire.
struct recorder {
  std::vector<stub_request> m_requests;

  void take(stub_request const& request) { m_requests.push_back(request); }

  [[nodiscard]] std::optional<stub_request> by_path(std::string_view path) const {
    for (auto const& request : m_requests) {
      if (path_of(request.m_target) == path) {
        return request;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::size_t count(std::string_view path) const {
    std::size_t total = 0;
    for (auto const& request : m_requests) {
      if (path_of(request.m_target) == path) {
        ++total;
      }
    }
    return total;
  }
};

inline std::string bearer_of(stub_request const& request) {
  auto const* value = qoderz::net::header_value(request.m_headers, "Authorization");
  if (value == nullptr) {
    return {};
  }
  constexpr std::string_view prefix = "Bearer ";
  if (value->size() <= prefix.size() || value->substr(0, prefix.size()) != prefix) {
    return {};
  }
  return value->substr(prefix.size());
}

struct fixture {
  fake_clock m_clock;
  site_definition const* m_site = nullptr;
  resolved_site m_resolved;
  bool m_ready = false;

  explicit fixture(stub_server& server, std::string_view site_id = "qoder") {
    auto site = qoderz::config::find_site(site_id);
    if (!site) {
      return;
    }
    m_site = *site;
    auto resolved = qoderz::config::resolve_site(*m_site, "");
    if (!resolved) {
      return;
    }
    auto overridden = qoderz::config::apply_loopback_override(*resolved, server.origin());
    if (!overridden) {
      return;
    }
    m_resolved = std::move(*overridden);
    m_ready = true;
  }
};

/// A launcher that records the URL and never starts anything: the tests must not open a browser.
struct quiet_launcher {
  std::string m_url;

  qoderz::result<void> operator()(std::string_view url) {
    m_url = std::string{url};
    return {};
  }
};

inline progress_callback collecting(std::vector<progress_event>& events) {
  return [&events](progress_event const& event) { events.push_back(event); };
}

} // namespace qoderz::test::device
