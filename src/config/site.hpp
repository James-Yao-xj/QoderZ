#pragma once

#include "common/error.hpp"
#include "common/strings.hpp"

#include <span>
#include <string>
#include <string_view>

namespace qoderz::config {

/// One row of the fixed site table. An empty `m_vpc_domain` means the site has no VPC hosts.
struct site_definition {
  std::string_view m_id;
  std::string_view m_name;
  std::string_view m_web;
  std::string_view m_openapi;
  std::string_view m_api;
  std::string_view m_client_id;
  std::string_view m_vpc_domain;
  bool m_device_chat = false;
};

[[nodiscard]] std::span<site_definition const> all_sites() noexcept;

/// Looks a site up by id, trimming surrounding whitespace and ignoring ASCII case. An empty or
/// unknown id is a configuration error that lists the available ids; there is no default site.
[[nodiscard]] result<site_definition const*> find_site(std::string_view id);

/// `^[a-z0-9]([a-z0-9-]*[a-z0-9])?$` — the shape every entry of the site table has.
[[nodiscard]] constexpr bool is_valid_site_id(std::string_view site) noexcept {
  if (site.empty() || site.front() == '-' || site.back() == '-') {
    return false;
  }
  for (auto const value : site) {
    if (!(value >= 'a' && value <= 'z') && !(value >= '0' && value <= '9') && value != '-') {
      return false;
    }
  }
  return true;
}

/// The canonical form of a site id: trimmed and ASCII-lowercased, matching how `find_site` compares
/// ids. Account keys and `defaults` are always stored under this form, so `QODER-CN` and `qoder-cn`
/// name the same accounts. Blank input yields an empty string.
[[nodiscard]] inline std::string normalize_site_id(std::string_view site) {
  return strings::ascii_lower_copy(strings::trim(site));
}
/// The three upstream bases one account talks to, after VPC and loopback overrides. The site id
/// and the effective VPC instance travel with the bases so callers never re-derive them.
struct resolved_site {
  std::string m_site_id;
  std::string m_web_base;
  std::string m_openapi_base;
  std::string m_api_base;
  std::string m_vpc;
};

/// Public hosts when `vpc` is blank, VPC hosts otherwise. A non-blank `vpc` on a site without VPC
/// support, or an instance that fails `normalize_vpc_instance`, is a configuration error: the
/// public hosts are never used as a fallback.
[[nodiscard]] result<resolved_site> resolve_site(site_definition const& site, std::string_view vpc);

/// Points every base at one loopback origin such as `http://127.0.0.1:18899`. The origin must be
/// plain `http://` with no user info, query, fragment or path; its host must be `127.0.0.0/8`,
/// `::1` or `localhost`. Anything else is a configuration error, because the client attaches a
/// credential to every upstream call.
[[nodiscard]] result<resolved_site> apply_loopback_override(resolved_site const& site,
                                                            std::string_view origin);

/// Validates one VPC instance and returns its canonical lowercase form; blank input means "no
/// VPC" and yields an empty string. `domain` is the site's VPC domain; an empty domain skips
/// suffix stripping, so only the bare instance is accepted.
[[nodiscard]] result<std::string> normalize_vpc_instance(std::string_view input,
                                                         std::string_view domain);

} // namespace qoderz::config
