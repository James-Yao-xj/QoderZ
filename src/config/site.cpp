#include "config/site.hpp"

#include "common/strings.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::config {
namespace {

constexpr std::string_view https_prefix = "https://";
constexpr std::string_view http_prefix = "http://";

constexpr std::array<site_definition, 2> site_table{{
    site_definition{
        .m_id = "qoder",
        .m_name = "Qoder",
        .m_web = "https://qoder.com",
        .m_openapi = "https://openapi.qoder.sh",
        .m_api = "https://api3.qoder.sh",
        .m_client_id = "732aef47-9cf2-46a2-95fe-4cebb5d0d1fa",
        .m_vpc_domain = "",
        .m_device_chat = false,
    },
    site_definition{
        .m_id = "qoder-cn",
        .m_name = "Qoder CN",
        .m_web = "https://qoder.cn",
        .m_openapi = "https://openapi.qoder.com.cn",
        .m_api = "https://gateway.qoder.com.cn",
        .m_client_id = "e883ade2-e6e3-4d6d-adf7-f92ceff5fdcb",
        .m_vpc_domain = "vpc.qoder.com.cn",
        .m_device_chat = true,
    },
}};

static_assert(site_table.size() == 2);
static_assert(site_table[0].m_vpc_domain.empty());
static_assert(site_table[1].m_vpc_domain == "vpc.qoder.com.cn");
static_assert(site_table[1].m_device_chat);

std::string available_site_ids() {
  std::string ids;
  for (auto const& site : site_table) {
    if (!ids.empty()) {
      ids += ", ";
    }
    ids += '\'';
    ids.append(site.m_id);
    ids += '\'';
  }
  return ids;
}

bool is_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

bool is_lower_alnum(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

bool is_ipv4_literal(std::string_view host) noexcept {
  std::size_t parts = 0;
  std::size_t start = 0;
  while (start <= host.size()) {
    auto const dot = host.find('.', start);
    auto const end = dot == std::string_view::npos ? host.size() : dot;
    auto const part = host.substr(start, end - start);
    if (part.empty() || part.size() > 3 || parts >= 4) {
      return false;
    }
    int value = 0;
    for (auto const digit : part) {
      if (!is_digit(digit)) {
        return false;
      }
      value = value * 10 + (digit - '0');
    }
    if (value > 255) {
      return false;
    }
    ++parts;
    if (dot == std::string_view::npos) {
      return parts == 4;
    }
    start = dot + 1;
  }
  return false;
}

bool is_valid_instance(std::string_view instance) noexcept {
  if (instance.empty() || !is_lower_alnum(instance.front()) || !is_lower_alnum(instance.back())) {
    return false;
  }
  for (auto const value : instance) {
    if (!is_lower_alnum(value) && value != '-') {
      return false;
    }
  }
  return true;
}

std::string_view strip_domain(std::string_view value, std::string_view domain) noexcept {
  if (domain.empty()) {
    return value;
  }
  constexpr std::array<std::string_view, 2> labels{"-openapi.", "-gateway."};
  for (auto const label : labels) {
    auto const length = label.size() + domain.size();
    if (value.size() > length && strings::ends_with(value, domain) &&
        value.substr(value.size() - length, label.size()) == label) {
      return value.substr(0, value.size() - length);
    }
  }
  if (value.size() > domain.size() + 1 && strings::ends_with(value, domain) &&
      value[value.size() - domain.size() - 1] == '.') {
    return value.substr(0, value.size() - domain.size() - 1);
  }
  return value;
}

// An absolute https URL whose path is empty or exactly "/", with no user info, query or
// fragment and no port other than 443.
std::optional<std::string_view> https_host(std::string_view value) {
  if (!strings::starts_with(value, https_prefix)) {
    return std::nullopt;
  }
  auto rest = value.substr(https_prefix.size());
  if (!rest.empty() && rest.back() == '/') {
    rest.remove_suffix(1);
  }
  auto authority = rest;
  auto path = std::string_view{};
  if (auto const slash = rest.find('/'); slash != std::string_view::npos) {
    authority = rest.substr(0, slash);
    path = rest.substr(slash);
  }
  if (authority.empty() || !path.empty()) {
    return std::nullopt;
  }
  if (authority.find('@') != std::string_view::npos ||
      authority.find('?') != std::string_view::npos ||
      authority.find('#') != std::string_view::npos) {
    return std::nullopt;
  }
  auto host = authority;
  if (auto const colon = authority.rfind(':'); colon != std::string_view::npos) {
    if (authority.substr(colon + 1) != "443") {
      return std::nullopt;
    }
    host = authority.substr(0, colon);
  }
  return host;
}

bool is_loopback_host(std::string_view host) noexcept {
  if (strings::equals_ignore_case(host, "localhost")) {
    return true;
  }
  if (host == "::1" || host == "[::1]") {
    return true;
  }
  return strings::starts_with(host, "127.") && is_ipv4_literal(host);
}

result<std::string> normalize_loopback_origin(std::string_view origin) {
  auto const trimmed = strings::trim(origin);
  auto const lowered = strings::ascii_lower_copy(trimmed);
  auto value = std::string_view{lowered};
  auto const reject = [&](std::string_view detail) {
    return std::unexpected(
        configuration_error("'" + std::string{trimmed} +
                            "' is not a valid upstream override: " + std::string{detail} + "."));
  };
  if (!strings::starts_with(value, http_prefix)) {
    return reject("use an http:// origin such as http://127.0.0.1:18899");
  }
  value.remove_prefix(http_prefix.size());
  if (!value.empty() && value.back() == '/') {
    value.remove_suffix(1);
  }
  if (value.empty()) {
    return reject("the origin has no host");
  }
  if (value.find('/') != std::string_view::npos) {
    return reject("the origin must not carry a path");
  }
  if (value.find('?') != std::string_view::npos) {
    return reject("the origin must not carry a query");
  }
  if (value.find('#') != std::string_view::npos) {
    return reject("the origin must not carry a fragment");
  }
  if (value.find('@') != std::string_view::npos) {
    return reject("the origin must not carry user info");
  }

  auto host = value;
  auto port = std::string_view{};
  if (value.front() == '[') {
    auto const close = value.find(']');
    if (close == std::string_view::npos) {
      return reject("the IPv6 host is missing a closing bracket");
    }
    host = value.substr(0, close + 1);
    if (close + 1 < value.size()) {
      if (value[close + 1] != ':') {
        return reject("the origin must be host:port");
      }
      port = value.substr(close + 2);
      if (port.empty()) {
        return reject("the origin must be host:port");
      }
    }
  } else if (auto const colon = value.find(':'); colon != std::string_view::npos) {
    host = value.substr(0, colon);
    port = value.substr(colon + 1);
    if (port.empty()) {
      return reject("the origin must be host:port");
    }
  }

  if (!port.empty()) {
    unsigned int number = 0;
    for (auto const digit : port) {
      if (!is_digit(digit)) {
        return reject("the port must be numeric");
      }
      number = number * 10 + static_cast<unsigned int>(digit - '0');
      if (number > 65535) {
        return reject("the port must be between 1 and 65535");
      }
    }
    if (number == 0) {
      return reject("the port must be between 1 and 65535");
    }
  }

  if (!is_loopback_host(host)) {
    return reject("the host must be loopback (127.0.0.0/8, ::1 or localhost)");
  }

  auto normalized = std::string{http_prefix} + std::string{host};
  if (!port.empty()) {
    normalized += ':';
    normalized += port;
  }
  return normalized;
}

} // namespace

std::span<site_definition const> all_sites() noexcept {
  return site_table;
}

result<site_definition const*> find_site(std::string_view id) {
  auto const wanted = strings::trim(id);
  if (wanted.empty()) {
    return std::unexpected(configuration_error(
        "A site id is required; available sites: " + available_site_ids() + "."));
  }
  for (auto const& site : site_table) {
    if (strings::equals_ignore_case(site.m_id, wanted)) {
      return &site;
    }
  }
  return std::unexpected(configuration_error("Unknown site '" + std::string{wanted} +
                                             "'; available sites: " + available_site_ids() + "."));
}

result<std::string> normalize_vpc_instance(std::string_view input, std::string_view domain) {
  auto const trimmed = strings::trim(input);
  if (trimmed.empty()) {
    return std::string{};
  }
  auto const lowered = strings::ascii_lower_copy(trimmed);
  auto const reject = [&](std::string_view detail) {
    return std::unexpected(
        configuration_error("'" + std::string{trimmed} +
                            "' is not a valid VPC instance: " + std::string{detail} + "."));
  };
  if (strings::starts_with(lowered, http_prefix)) {
    return reject("use the instance name, not a plain-http URL");
  }
  auto value = std::string_view{lowered};
  if (strings::starts_with(value, https_prefix)) {
    auto const host = https_host(value);
    if (!host) {
      return reject("an https form must be an absolute https URL without user info, query, "
                    "fragment or path");
    }
    value = *host;
  }
  if (is_ipv4_literal(value)) {
    return reject("an IP address cannot name a VPC instance");
  }
  value = strip_domain(value, strings::ascii_lower_copy(strings::trim(domain)));
  if (value.empty()) {
    return reject("the instance name is empty");
  }
  if (value.find('.') != std::string_view::npos) {
    return reject("the instance must not contain '.'");
  }
  if (!is_valid_instance(value)) {
    return reject("use lowercase letters, digits or '-', and do not start or end with '-'");
  }
  return std::string{value};
}

result<resolved_site> resolve_site(site_definition const& site, std::string_view vpc) {
  resolved_site resolved{
      .m_site_id = std::string{site.m_id},
      .m_web_base = std::string{site.m_web},
      .m_openapi_base = std::string{site.m_openapi},
      .m_api_base = std::string{site.m_api},
      .m_vpc = std::string{},
  };
  if (strings::trim(vpc).empty()) {
    return resolved;
  }
  if (site.m_vpc_domain.empty()) {
    return std::unexpected(
        configuration_error("Site '" + std::string{site.m_id} +
                            "' does not support VPC hosts; an account on this site cannot "
                            "store a VPC instance."));
  }
  auto const instance = normalize_vpc_instance(vpc, site.m_vpc_domain);
  if (!instance) {
    return std::unexpected(instance.error());
  }
  auto const domain = std::string{site.m_vpc_domain};
  auto const host = std::string{https_prefix} + *instance;
  resolved.m_vpc = *instance;
  resolved.m_web_base = host + "." + domain;
  resolved.m_openapi_base = host + "-openapi." + domain;
  resolved.m_api_base = host + "-gateway." + domain;
  return resolved;
}

result<resolved_site> apply_loopback_override(resolved_site const& site, std::string_view origin) {
  auto const normalized = normalize_loopback_origin(origin);
  if (!normalized) {
    return std::unexpected(normalized.error());
  }
  resolved_site overridden = site;
  overridden.m_web_base = *normalized;
  overridden.m_openapi_base = *normalized;
  overridden.m_api_base = *normalized;
  return overridden;
}

} // namespace qoderz::config
