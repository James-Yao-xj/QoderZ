#include "common/error.hpp"
#include "config/site.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

using qoderz::config::all_sites;
using qoderz::config::apply_loopback_override;
using qoderz::config::find_site;
using qoderz::config::normalize_vpc_instance;
using qoderz::config::resolve_site;
using qoderz::config::site_definition;

namespace {

constexpr std::string_view domain = "vpc.qoder.com.cn";

site_definition const& site_of(std::string_view id) {
  auto const found = find_site(id);
  REQUIRE(found.has_value());
  return **found;
}

} // namespace

TEST_CASE("the site table lists the two public sites", "[config][site]") {
  auto const sites = all_sites();
  REQUIRE(sites.size() == 2);
  CHECK(sites[0].m_id == "qoder");
  CHECK(sites[1].m_id == "qoder-cn");

  auto const& international = sites[0];
  CHECK(international.m_name == "Qoder");
  CHECK(international.m_web == "https://qoder.com");
  CHECK(international.m_openapi == "https://openapi.qoder.sh");
  CHECK(international.m_api == "https://api3.qoder.sh");
  CHECK(international.m_client_id == "732aef47-9cf2-46a2-95fe-4cebb5d0d1fa");
  CHECK(international.m_vpc_domain.empty());
  CHECK_FALSE(international.m_device_chat);

  auto const& china = sites[1];
  CHECK(china.m_name == "Qoder CN");
  CHECK(china.m_web == "https://qoder.cn");
  CHECK(china.m_openapi == "https://openapi.qoder.com.cn");
  CHECK(china.m_api == "https://gateway.qoder.com.cn");
  CHECK(china.m_client_id == "e883ade2-e6e3-4d6d-adf7-f92ceff5fdcb");
  CHECK(china.m_vpc_domain == domain);
  CHECK(china.m_device_chat);
}

TEST_CASE("find_site trims and ignores case", "[config][site]") {
  CHECK(site_of("qoder").m_id == "qoder");
  CHECK(site_of(" QODER ").m_id == "qoder");
  CHECK(site_of("qoder-cn").m_id == "qoder-cn");
  CHECK(site_of("Qoder-CN").m_id == "qoder-cn");
}

TEST_CASE("find_site rejects empty and unknown ids", "[config][site]") {
  for (auto const id : {std::string_view{""}, std::string_view{"  "}}) {
    auto const found = find_site(id);
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().m_kind == qoderz::error_kind::configuration);
    CHECK(found.error().m_message.find("qoder-cn") != std::string::npos);
  }

  auto const unknown = find_site("qoder-eu");
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().m_kind == qoderz::error_kind::configuration);
  CHECK(unknown.error().m_message.find("qoder-eu") != std::string::npos);
  CHECK(unknown.error().m_message.find("qoder-cn") != std::string::npos);
}

TEST_CASE("blank VPC input means no VPC", "[config][site]") {
  for (auto const input :
       {std::string_view{""}, std::string_view{"   "}, std::string_view{"\t\n"}}) {
    auto const normalized = normalize_vpc_instance(input, domain);
    REQUIRE(normalized.has_value());
    CHECK(normalized->empty());
  }
}

TEST_CASE("normalize_vpc_instance accepts the documented forms", "[config][site]") {
  std::vector<std::string_view> const inputs{
      "acme",
      "  acme  ",
      "ACME",
      "acme.vpc.qoder.com.cn",
      "https://acme.vpc.qoder.com.cn",
      "https://acme.vpc.qoder.com.cn/",
      "https://acme.vpc.qoder.com.cn:443",
      "https://acme.vpc.qoder.com.cn:443/",
      "acme-openapi.vpc.qoder.com.cn",
      "https://acme-openapi.vpc.qoder.com.cn/",
      "acme-gateway.vpc.qoder.com.cn",
      "https://acme-gateway.vpc.qoder.com.cn",
      "acme-1",
      "a",
      "0abc",
  };
  for (auto const input : inputs) {
    INFO("input: " << input);
    auto const normalized = normalize_vpc_instance(input, domain);
    REQUIRE(normalized.has_value());
    CHECK(normalized->find('.') == std::string::npos);
    if (input == "acme" || input == "  acme  " || input == "ACME") {
      CHECK(*normalized == "acme");
    }
    if (input == "https://acme.vpc.qoder.com.cn:443" ||
        input == "https://acme.vpc.qoder.com.cn:443/") {
      CHECK(*normalized == "acme");
    }
    if (input == "acme-1") {
      CHECK(*normalized == "acme-1");
    }
  }
}

TEST_CASE("normalize_vpc_instance rejects unsupported forms", "[config][site]") {
  std::vector<std::string_view> const inputs{
      "http://acme.vpc.qoder.com.cn",
      "HTTP://acme.vpc.qoder.com.cn",
      "10.0.0.1",
      "https://10.0.0.1",
      "acme.example.com",
      "https://acme.example.com",
      "https://acme.vpc.qoder.com.cn/api",
      "https://acme.vpc.qoder.com.cn?x=1",
      "https://acme.vpc.qoder.com.cn#frag",
      "https://user@acme.vpc.qoder.com.cn",
      "https://acme.vpc.qoder.com.cn:8443",
      "acme.vpc.qoder.com.cn:8443",
      "bad_instance",
      "-acme",
      "acme-",
      "acme..cn",
      ".acme",
      "https://",
  };
  for (auto const input : inputs) {
    INFO("input: " << input);
    auto const normalized = normalize_vpc_instance(input, domain);
    REQUIRE_FALSE(normalized.has_value());
    CHECK(normalized.error().m_kind == qoderz::error_kind::configuration);
    CHECK(normalized.error().m_message.find("VPC") != std::string::npos);
  }
}

TEST_CASE("normalize_vpc_instance without a domain only accepts the bare instance",
          "[config][site]") {
  auto const bare = normalize_vpc_instance("acme", "");
  REQUIRE(bare.has_value());
  CHECK(*bare == "acme");

  auto const qualified = normalize_vpc_instance("acme.vpc.qoder.com.cn", "");
  CHECK_FALSE(qualified.has_value());
}

TEST_CASE("resolve_site keeps the public hosts without a VPC", "[config][site]") {
  auto const resolved = resolve_site(site_of("qoder-cn"), "   ");
  REQUIRE(resolved.has_value());
  CHECK(resolved->m_site_id == "qoder-cn");
  CHECK(resolved->m_web_base == "https://qoder.cn");
  CHECK(resolved->m_openapi_base == "https://openapi.qoder.com.cn");
  CHECK(resolved->m_api_base == "https://gateway.qoder.com.cn");
  CHECK(resolved->m_vpc.empty());
}

TEST_CASE("resolve_site rewrites every host for a VPC", "[config][site]") {
  auto const resolved = resolve_site(site_of("qoder-cn"), " https://acme.vpc.qoder.com.cn/ ");
  REQUIRE(resolved.has_value());
  CHECK(resolved->m_site_id == "qoder-cn");
  CHECK(resolved->m_vpc == "acme");
  CHECK(resolved->m_web_base == "https://acme.vpc.qoder.com.cn");
  CHECK(resolved->m_openapi_base == "https://acme-openapi.vpc.qoder.com.cn");
  CHECK(resolved->m_api_base == "https://acme-gateway.vpc.qoder.com.cn");
}

TEST_CASE("resolve_site rejects a VPC on a site without VPC support", "[config][site]") {
  auto const resolved = resolve_site(site_of("qoder"), "acme");
  REQUIRE_FALSE(resolved.has_value());
  CHECK(resolved.error().m_kind == qoderz::error_kind::configuration);
  CHECK(resolved.error().m_message.find("VPC") != std::string::npos);
}

TEST_CASE("resolve_site rejects an invalid instance instead of using public hosts",
          "[config][site]") {
  auto const resolved = resolve_site(site_of("qoder-cn"), "bad_instance");
  REQUIRE_FALSE(resolved.has_value());
  CHECK(resolved.error().m_kind == qoderz::error_kind::configuration);
  CHECK(resolved.error().m_message.find("bad_instance") != std::string::npos);
}

TEST_CASE("apply_loopback_override points every base at one origin", "[config][site]") {
  auto const base = resolve_site(site_of("qoder-cn"), "acme");
  REQUIRE(base.has_value());

  std::vector<std::pair<std::string_view, std::string_view>> const accepted{
      {"http://127.0.0.1:18899", "http://127.0.0.1:18899"},
      {"http://127.0.0.1:18899/", "http://127.0.0.1:18899"},
      {"http://127.5.5.5:80", "http://127.5.5.5:80"},
      {"http://localhost:8080", "http://localhost:8080"},
      {"http://LOCALHOST:8080", "http://localhost:8080"},
      {"http://[::1]:9000", "http://[::1]:9000"},
      {"http://127.0.0.1", "http://127.0.0.1"},
  };
  for (auto const& [origin, expected] : accepted) {
    INFO("origin: " << origin);
    auto const overridden = apply_loopback_override(*base, origin);
    REQUIRE(overridden.has_value());
    CHECK(overridden->m_web_base == expected);
    CHECK(overridden->m_openapi_base == expected);
    CHECK(overridden->m_api_base == expected);
    CHECK(overridden->m_site_id == "qoder-cn");
    CHECK(overridden->m_vpc == "acme");
  }
}

TEST_CASE("apply_loopback_override rejects non-loopback origins", "[config][site]") {
  auto const base = resolve_site(site_of("qoder-cn"), "");
  REQUIRE(base.has_value());

  std::vector<std::string_view> const rejected{
      "https://127.0.0.1:18899",
      "http://192.168.0.10:18880",
      "http://gateway.qoder.com.cn",
      "http://localhost.evil.example:80",
      "http://[::2]:9000",
      "http://127.0.0.1:18899/path",
      "http://127.0.0.1:18899?x=1",
      "http://127.0.0.1:18899#frag",
      "http://user@127.0.0.1:18899",
      "http://127.0.0.1:0",
      "http://127.0.0.1:99999",
      "http://127.0.0.1:port",
      "127.0.0.1:18899",
      "http://",
  };
  for (auto const origin : rejected) {
    INFO("origin: " << origin);
    auto const overridden = apply_loopback_override(*base, origin);
    REQUIRE_FALSE(overridden.has_value());
    CHECK(overridden.error().m_kind == qoderz::error_kind::configuration);
  }
}

TEST_CASE("apply_loopback_override says why a host is refused", "[config][site]") {
  auto const base = resolve_site(site_of("qoder"), "");
  REQUIRE(base.has_value());

  auto const overridden = apply_loopback_override(*base, "http://example.com:80");
  REQUIRE_FALSE(overridden.has_value());
  CHECK(overridden.error().m_message.find("loopback") != std::string::npos);
}

TEST_CASE("site ids normalize to the canonical lowercase form", "[config][site]") {
  using qoderz::config::is_valid_site_id;
  using qoderz::config::normalize_site_id;

  CHECK(normalize_site_id(" Qoder-CN ") == "qoder-cn");
  CHECK(normalize_site_id("qoder") == "qoder");
  CHECK(normalize_site_id("   ").empty());

  CHECK(is_valid_site_id("qoder"));
  CHECK(is_valid_site_id("qoder-cn"));
  CHECK(is_valid_site_id("cn2"));
  for (auto const invalid :
       {std::string_view{""}, std::string_view{"Qoder"}, std::string_view{"qoder_cn"},
        std::string_view{"qoder/cn"}, std::string_view{"-qoder"}, std::string_view{"qoder-"},
        std::string_view{"qoder.cn"}}) {
    INFO("id: " << invalid);
    CHECK_FALSE(is_valid_site_id(invalid));
  }
}
