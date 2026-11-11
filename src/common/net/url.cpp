#include "common/net/url.hpp"

#include "common/strings.hpp"

#include <algorithm>
#include <boost/url.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::net {
namespace {

constexpr std::string_view http_scheme = "http";
constexpr std::string_view https_scheme = "https";
constexpr std::string_view http_port = "80";
constexpr std::string_view https_port = "443";
constexpr int max_port = 65535;

std::unexpected<error> url_error(std::string_view what) {
  return std::unexpected(
      bad_request_error("The URL is not usable for an HTTP request: " + std::string{what}));
}

/// Ports are compared rather than trusted verbatim: `0080` and `80` have to reach the same pool
/// bucket, and anything above 65535 has no TCP meaning.
std::optional<std::string> normalize_port(std::string_view text, std::string_view fallback) {
  if (text.empty()) {
    return std::string{fallback};
  }
  std::uint32_t value = 0;
  for (auto const digit : text) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint32_t>(digit - '0');
    if (value > static_cast<std::uint32_t>(max_port)) {
      return std::nullopt;
    }
  }
  return std::to_string(value);
}

/// `[::1]` → `::1`; a host without brackets is only lowercased.
std::string strip_ipv6_brackets(std::string_view host) {
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    return strings::ascii_lower_copy(host.substr(1, host.size() - 2));
  }
  return strings::ascii_lower_copy(host);
}

std::string assemble_target(std::string_view path, std::string_view query) {
  std::string target{path.empty() ? std::string_view{"/"} : path};
  if (!query.empty()) {
    target.push_back('?');
    target.append(query);
  }
  return target;
}

} // namespace

result<parsed_url> parse_url(std::string_view url) {
  auto parsed = boost::urls::parse_uri(url);
  if (!parsed) {
    return url_error("it is not an absolute URL with http or https in front of the host");
  }
  auto const& view = *parsed;

  auto const scheme = strings::ascii_lower_copy(view.scheme());
  if (scheme != http_scheme && scheme != https_scheme) {
    return url_error("only the http and https schemes are supported");
  }
  if (view.has_userinfo()) {
    return url_error("userinfo in front of the host is not supported; send a header instead");
  }
  if (view.has_fragment()) {
    return url_error("fragments are not sent to a server and must be rejected, not dropped");
  }
  if (!view.has_authority() || view.host().empty()) {
    return url_error("the host is empty");
  }
  bool const tls = scheme == https_scheme;
  auto port = normalize_port(view.port(), tls ? https_port : http_port);
  if (!port) {
    return url_error("the port is not a number between 0 and 65535");
  }

  parsed_url result;
  result.m_scheme = scheme;
  result.m_host = strip_ipv6_brackets(view.host());
  result.m_port = std::move(*port);
  result.m_target = assemble_target(view.encoded_path(), view.encoded_query());
  result.m_tls = tls;
  if (result.m_host.empty()) {
    return url_error("the host is empty");
  }
  return result;
}

bool is_loopback_host(std::string_view host) noexcept {
  if (strings::equals_ignore_case(host, "localhost")) {
    return true;
  }
  if (!host.empty() && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
    return true;
  }
  auto const first = host.find('.');
  if (first == std::string_view::npos) {
    return false;
  }
  return host.substr(0, first) == "127" &&
         host.find_first_not_of("0123456789.") == std::string_view::npos;
}

std::string origin_of(parsed_url const& url) {
  std::string origin;
  origin.reserve(url.m_scheme.size() + url.m_host.size() + url.m_port.size() + 4);
  origin.append(url.m_scheme);
  origin.append("://");
  bool const is_ipv6 =
      url.m_host.find(':') != std::string::npos && url.m_host.find('/') == std::string::npos;
  if (is_ipv6) {
    origin.push_back('[');
    origin.append(url.m_host);
    origin.push_back(']');
  } else {
    origin.append(url.m_host);
  }
  origin.push_back(':');
  origin.append(url.m_port);
  return origin;
}

std::string host_field_of(parsed_url const& url) {
  bool const is_ipv6 = url.m_host.find(':') != std::string::npos;
  bool const default_port = (url.m_scheme == http_scheme && url.m_port == http_port) ||
                            (url.m_scheme == https_scheme && url.m_port == https_port);
  std::string host;
  host.reserve(url.m_host.size() + url.m_port.size() + 4);
  if (is_ipv6) {
    host.push_back('[');
  }
  host.append(url.m_host);
  if (is_ipv6) {
    host.push_back(']');
  }
  if (!default_port) {
    host.push_back(':');
    host.append(url.m_port);
  }
  return host;
}

} // namespace qoderz::net
