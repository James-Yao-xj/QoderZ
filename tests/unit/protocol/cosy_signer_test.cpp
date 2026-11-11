#include "common/codec/base64.hpp"
#include "common/codec/hash.hpp"
#include "protocol/cosy_signer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view url =
    "https://gateway.qoder.com.cn/algo/api/v2/service/pro/sse/agent_chat_generation"
    "?FetchKeys=llm_model_result&AgentId=agent_common&Encode=1";
constexpr std::int64_t timestamp = 1'760'000'000;
constexpr std::string_view wire_body = R"({"request_id":"r1","stream":true})";
constexpr std::string_view aes_key = "0123456789abcdef";
constexpr std::string_view request_id = "0123456789abcdef0123456789abcdef";

qoderz::protocol::cosy_identity make_identity() {
  return qoderz::protocol::cosy_identity{
      .m_uid = "uid-1",
      .m_name = "Ada Lovelace",
      .m_email = "ada@example.com",
      .m_access_token = "jt-access-token",
      .m_machine_id = "machine-42",
      .m_machine_os = "x86_64_linux",
  };
}

class fake_key_wrapper final : public qoderz::protocol::rsa_key_wrapper {
public:
  explicit fake_key_wrapper(std::string wrapped) : m_wrapped{std::move(wrapped)} {}

  [[nodiscard]] qoderz::result<std::string> wrap(std::string_view key) const override {
    m_seen.push_back(std::string{key});
    return m_wrapped;
  }

  [[nodiscard]] std::vector<std::string> const& seen() const noexcept { return m_seen; }

private:
  std::string m_wrapped;
  mutable std::vector<std::string> m_seen;
};

std::string fake_wrapped_key() {
  return qoderz::codec::base64_encode(std::string(128, '\x5a'));
}

bool is_lower_hex(std::string_view text, std::size_t length) {
  if (text.size() != length) {
    return false;
  }
  for (char const character : text) {
    auto const is_digit = character >= '0' && character <= '9';
    auto const is_letter = character >= 'a' && character <= 'f';
    if (!is_digit && !is_letter) {
      return false;
    }
  }
  return true;
}

std::string const* find_header(qoderz::protocol::cosy_headers const& headers,
                               std::string_view name) {
  for (auto const& header : headers) {
    if (header.m_name == name) {
      return &header.m_value;
    }
  }
  return nullptr;
}

struct authorization_parts {
  std::string m_payload;
  std::string m_signature;
};

authorization_parts split_authorization(std::string const& authorization) {
  REQUIRE(authorization.starts_with("Bearer COSY."));
  auto const rest = authorization.substr(std::string_view{"Bearer COSY."}.size());
  auto const dot = rest.find('.');
  REQUIRE(dot != std::string::npos);
  return authorization_parts{rest.substr(0, dot), rest.substr(dot + 1)};
}

} // namespace

TEST_CASE("encrypt_info matches the committed fixed vector", "[protocol][cosy]") {
  // CBC, PKCS7, IV = key. The expectation is pinned by docs/protocol/qoder-auth.md §5.3.
  auto const cipher = qoderz::protocol::encrypt_info(make_identity(), aes_key);
  REQUIRE(cipher.has_value());
  CHECK(*cipher ==
        "cME0higR7RPuNL3LZJjqAexNgGJFc4QH+atZ/vo5QSKeChOvcZ6GoEgMij7vf0iv+0gnoLj14/LuhdGWd0Iv"
        "huirL6rOIxnN06ymuZtg9bRlhnISX5u9wGCOsP0ham1FZ8kCmHo+fhXOjCmExtOmjjNAYc5x3Yd6YAJ2iXZIdS8=");
}

TEST_CASE("encrypt_info rejects a key of the wrong length", "[protocol][cosy]") {
  auto const cipher = qoderz::protocol::encrypt_info(make_identity(), "short");
  REQUIRE_FALSE(cipher.has_value());
  CHECK(cipher.error().m_kind == qoderz::error_kind::protocol);
}

TEST_CASE("signed_path drops the algo prefix and the query", "[protocol][cosy]") {
  CHECK(qoderz::protocol::signed_path(url) == "/api/v2/service/pro/sse/agent_chat_generation");
  CHECK(qoderz::protocol::signed_path("https://api3.qoder.sh/v1/models?Encode=1#top") ==
        "/v1/models");
  CHECK(qoderz::protocol::signed_path("https://qoder.com") == "/");
  CHECK(qoderz::protocol::signed_path("https://qoder.com?x=1") == "/");
  CHECK(qoderz::protocol::signed_path("https://qoder.com/algo").empty());
  CHECK(qoderz::protocol::signed_path("https://qoder.com/algo/x") == "/x");
  CHECK_THROWS_AS(qoderz::protocol::signed_path("  "), std::invalid_argument);
  CHECK_THROWS_AS(qoderz::protocol::signed_path("api/v2/models"), std::invalid_argument);
}

TEST_CASE("machine_os reports the build target", "[protocol][cosy]") {
  auto const os = qoderz::protocol::machine_os();
  auto const separator = os.rfind('_');
  REQUIRE(separator != std::string::npos);

  auto const arch = os.substr(0, separator);
  auto const platform = os.substr(separator + 1);
  CHECK((arch == "x86_64" || arch == "aarch64" || arch == "x86" || arch == "other"));
  CHECK((platform == "win32" || platform == "darwin" || platform == "linux" ||
         platform == "freebsd" || platform == "unknown"));
  CHECK_FALSE(os.empty());
}

TEST_CASE("build_signature_input joins with newlines", "[protocol][cosy]") {
  CHECK(qoderz::protocol::build_signature_input("p", "w", 1760000000, "b", "/x") ==
        "p\nw\n1760000000\nb\n/x");
  CHECK(qoderz::protocol::build_signature_input("p", "w", 0, "", "/") == "p\nw\n0\n\n/");
}

TEST_CASE("create_cosy_headers emits the full header set", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  auto const material =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, wrapper,
                                            std::string{aes_key}, std::string{request_id});
  REQUIRE(material.has_value());

  CHECK(material->m_aes_key == aes_key);
  CHECK(wrapper.seen().size() == 1);
  CHECK(wrapper.seen().front() == aes_key);

  CHECK(material->m_headers.size() == 19);
  std::vector<std::string> const expected_names{"Accept",
                                                "Accept-Encoding",
                                                "Content-Type",
                                                "Authorization",
                                                "Cosy-Business-Product",
                                                "Cosy-Business-Type",
                                                "Cosy-ClientIp",
                                                "Cosy-ClientType",
                                                "Cosy-Data-Policy",
                                                "Cosy-Date",
                                                "Cosy-Key",
                                                "Cosy-MachineId",
                                                "Cosy-MachineToken",
                                                "Cosy-MachineType",
                                                "Cosy-MachineOS",
                                                "Cosy-Scene",
                                                "Cosy-User",
                                                "Cosy-Version",
                                                "Login-Version"};

  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    CAPTURE(index);
    CHECK(material->m_headers[index].m_name == expected_names[index]);
  }

  auto const value = [&](std::string_view name) -> std::string {
    auto const* found = find_header(material->m_headers, name);
    REQUIRE(found != nullptr);
    return *found;
  };

  CHECK(value("Accept") == "application/json");
  CHECK(value("Accept-Encoding") == "identity");
  CHECK(value("Content-Type") == "application/json");
  CHECK(value("Cosy-Business-Product") == "app");
  CHECK(value("Cosy-Business-Type") == "agent");
  CHECK(value("Cosy-ClientIp") == "machine-42");
  CHECK(value("Cosy-ClientType") == "10");
  CHECK(value("Cosy-Data-Policy") == "disagree");
  CHECK(value("Cosy-Date") == "1760000000");
  CHECK(value("Cosy-Key") == fake_wrapped_key());
  CHECK(value("Cosy-MachineId") == "machine-42");
  CHECK(value("Cosy-MachineToken") == "machine-42");
  CHECK(value("Cosy-MachineType") == "5");
  CHECK(value("Cosy-MachineOS") == "x86_64_linux");
  CHECK(value("Cosy-Scene") == "app");
  CHECK(value("Cosy-User") == "uid-1");
  CHECK(value("Cosy-Version") == "1.1.69");
  CHECK(value("Login-Version") == "v2");
  CHECK(value("Authorization") == material->m_authorization);
  CHECK(material->m_signed_path == "/api/v2/service/pro/sse/agent_chat_generation");
}

TEST_CASE("create_cosy_headers produces a recomputable signature", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  auto const material =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, wrapper,
                                            std::string{aes_key}, std::string{request_id});
  REQUIRE(material.has_value());

  auto const parts = split_authorization(material->m_authorization);
  CHECK(parts.m_payload == material->m_payload);
  CHECK(parts.m_signature == material->m_signature);
  CHECK(material->m_authorization ==
        "Bearer COSY." + material->m_payload + "." + material->m_signature);
  CHECK(is_lower_hex(material->m_signature, 32));

  auto const input = qoderz::protocol::build_signature_input(
      material->m_payload, material->m_wrapped_key, timestamp, wire_body, material->m_signed_path);
  CHECK(qoderz::codec::md5_hex(input) == material->m_signature);
}

TEST_CASE("create_cosy_headers wraps the cosy envelope", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  auto const material =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, wrapper,
                                            std::string{aes_key}, std::string{request_id});
  REQUIRE(material.has_value());

  auto const payload = qoderz::codec::base64_decode(material->m_payload);
  REQUIRE(payload.has_value());

  auto const document = nlohmann::json::parse(*payload);
  CHECK(document.at("version") == "v1");
  CHECK(document.at("requestId") == request_id);
  CHECK(document.at("cosyVersion") == "1.1.69");
  CHECK(document.at("ideVersion") == "");
  CHECK(document.at("info") == material->m_info);

  auto const info = qoderz::codec::base64_decode(material->m_info);
  REQUIRE(info.has_value());
  CHECK(info->size() % 16 == 0);
  CHECK_FALSE(info->empty());

  auto const wrapped = qoderz::codec::base64_decode(material->m_wrapped_key);
  REQUIRE(wrapped.has_value());
  CHECK(wrapped->size() == 128);
}

TEST_CASE("create_cosy_headers randomises key and request id", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  auto const first =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, wrapper);
  auto const second =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, wrapper);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  CHECK(first->m_aes_key != second->m_aes_key);
  CHECK(is_lower_hex(first->m_aes_key, 16));
  CHECK(first->m_info != second->m_info);
  CHECK(first->m_authorization != second->m_authorization);

  auto const payload = qoderz::codec::base64_decode(first->m_payload);
  REQUIRE(payload.has_value());
  auto const document = nlohmann::json::parse(*payload);
  CHECK(is_lower_hex(document.at("requestId").get<std::string>(), 32));
}

TEST_CASE("create_cosy_headers rejects a relative url as configuration", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};

  // Only the shape of the URL is checked, never its contents.
  for (std::string_view const bad_url :
       {"api/v2/models", "gateway.qoder.com.cn/algo/x", "://gateway.qoder.com.cn/algo/x"}) {
    CAPTURE(bad_url);
    auto const material = qoderz::protocol::create_cosy_headers(
        bad_url, make_identity(), wire_body, timestamp, wrapper, std::string{aes_key},
        std::string{request_id});
    REQUIRE_FALSE(material.has_value());
    CHECK(material.error().m_kind == qoderz::error_kind::configuration);
    CHECK(material.error().m_message.find("not an absolute URL") != std::string::npos);
    // The message is fixed text: it explains the shape problem without echoing the URL back.
    CHECK(material.error().m_message ==
          "cosy: the request URL is not an absolute URL (expected scheme://host/...)");
    CHECK(material.error().m_message.find(bad_url) == std::string::npos);
  }

  for (std::string_view const blank : {"", "   ", "\t"}) {
    auto const material = qoderz::protocol::create_cosy_headers(blank, make_identity(), wire_body,
                                                                timestamp, wrapper);
    REQUIRE_FALSE(material.has_value());
    CHECK(material.error().m_kind == qoderz::error_kind::configuration);
    CHECK(material.error().m_message.find("must not be blank") != std::string::npos);
  }

  CHECK(wrapper.seen().empty());
}

TEST_CASE("pem_rsa_key_wrapper is created through make_unique", "[protocol][cosy]") {
  // `create()` owns the key via `std::make_unique`; the only observable contract is that two
  // independently created wrappers are distinct objects that both wrap.
  auto first = qoderz::protocol::pem_rsa_key_wrapper::create();
  auto second = qoderz::protocol::pem_rsa_key_wrapper::create();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(*first != *second);
  CHECK((*first)->wrap(aes_key).has_value());
  CHECK((*second)->wrap(aes_key).has_value());
}

TEST_CASE("create_cosy_headers rejects a broken identity", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  auto identity = make_identity();

  identity.m_uid.clear();
  auto const without_uid =
      qoderz::protocol::create_cosy_headers(url, identity, wire_body, timestamp, wrapper);
  REQUIRE_FALSE(without_uid.has_value());
  CHECK(without_uid.error().m_kind == qoderz::error_kind::configuration);
  CHECK(without_uid.error().m_message.find("run 'qoderz login' again") != std::string::npos);

  identity = make_identity();
  identity.m_uid = "   ";
  auto const blank_uid =
      qoderz::protocol::create_cosy_headers(url, identity, wire_body, timestamp, wrapper);
  REQUIRE_FALSE(blank_uid.has_value());
  CHECK(blank_uid.error().m_kind == qoderz::error_kind::configuration);

  identity = make_identity();
  identity.m_machine_id = "\t ";
  auto const without_machine =
      qoderz::protocol::create_cosy_headers(url, identity, wire_body, timestamp, wrapper);
  REQUIRE_FALSE(without_machine.has_value());
  CHECK(without_machine.error().m_kind == qoderz::error_kind::configuration);
}

TEST_CASE("create_cosy_headers validates the injected key", "[protocol][cosy]") {
  fake_key_wrapper wrapper{fake_wrapped_key()};
  for (std::string_view const key :
       {"0123456789abcde", "0123456789ABCDEF", "0123456789abcdeg", "0123456789abcdef0"}) {
    CAPTURE(key);
    auto const material = qoderz::protocol::create_cosy_headers(
        url, make_identity(), wire_body, timestamp, wrapper, std::string{key});
    REQUIRE_FALSE(material.has_value());
    CHECK(material.error().m_kind == qoderz::error_kind::configuration);
  }
}

TEST_CASE("the embedded rsa key wraps into 128 random bytes", "[protocol][cosy]") {
  auto wrapper = qoderz::protocol::pem_rsa_key_wrapper::create();
  REQUIRE(wrapper.has_value());

  auto const first = (*wrapper)->wrap(aes_key);
  auto const second = (*wrapper)->wrap(aes_key);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(*first != *second);

  auto const first_bytes = qoderz::codec::base64_decode(*first);
  auto const second_bytes = qoderz::codec::base64_decode(*second);
  REQUIRE(first_bytes.has_value());
  REQUIRE(second_bytes.has_value());
  CHECK(first_bytes->size() == 128);
  CHECK(second_bytes->size() == 128);
  CHECK(*first != *second);
}

TEST_CASE("the pem wrapper drives create_cosy_headers", "[protocol][cosy]") {
  auto wrapper = qoderz::protocol::pem_rsa_key_wrapper::create();
  REQUIRE(wrapper.has_value());

  auto const material =
      qoderz::protocol::create_cosy_headers(url, make_identity(), wire_body, timestamp, **wrapper);
  REQUIRE(material.has_value());
  CHECK(material->m_wrapped_key.size() > 0);

  auto const wrapped = qoderz::codec::base64_decode(material->m_wrapped_key);
  REQUIRE(wrapped.has_value());
  CHECK(wrapped->size() == 128);
}
