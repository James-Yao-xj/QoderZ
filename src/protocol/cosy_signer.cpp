#include "protocol/cosy_signer.hpp"

#include "common/codec/base64.hpp"
#include "common/codec/hash.hpp"
#include "common/codec/random.hpp"
#include "common/strings.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::protocol {
namespace {

constexpr std::string_view embedded_public_key = R"(-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDA8iMH5c02LilrsERw9t6Pv5Nc
4k6Pz1EaDicBMpdpxKduSZu5OANqUq8er4GM95omAGIOPOh+Nx0spthYA2BqGz+l
6HRkPJ7S236FZz73In/KVuLnwI8JJ2CbuJap8kvheCCZpmAWpb/cPx/3Vr/J6I17
XcW+ML9FoCI6AOvOzwIDAQAB
-----END PUBLIC KEY-----
)";

constexpr std::size_t aes_key_length = 16;
constexpr std::size_t wrapped_key_length = 128;

struct bio_deleter {
  void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct pkey_deleter {
  void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct cipher_ctx_deleter {
  void operator()(EVP_CIPHER_CTX* context) const noexcept { EVP_CIPHER_CTX_free(context); }
};

struct pkey_ctx_deleter {
  void operator()(EVP_PKEY_CTX* context) const noexcept { EVP_PKEY_CTX_free(context); }
};

using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;

bool is_lower_hex(std::string_view text) noexcept {
  for (char const character : text) {
    auto const is_digit = character >= '0' && character <= '9';
    auto const is_letter = character >= 'a' && character <= 'f';
    if (!is_digit && !is_letter) {
      return false;
    }
  }
  return true;
}

} // namespace

struct pem_rsa_key_wrapper::impl {
  pkey_ptr m_key;
};

pem_rsa_key_wrapper::pem_rsa_key_wrapper(std::unique_ptr<impl> state) : m_impl{std::move(state)} {}

pem_rsa_key_wrapper::~pem_rsa_key_wrapper() = default;

result<std::unique_ptr<pem_rsa_key_wrapper>> pem_rsa_key_wrapper::create() {
  std::unique_ptr<BIO, bio_deleter> bio{
      BIO_new_mem_buf(embedded_public_key.data(), static_cast<int>(embedded_public_key.size()))};
  if (!bio) {
    return std::unexpected(
        make_error(error_kind::internal, "openssl: cannot open the embedded public key"));
  }

  pkey_ptr key{PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr)};
  if (!key) {
    return std::unexpected(
        make_error(error_kind::internal, "openssl: cannot read the embedded public key"));
  }

  auto state = std::make_unique<impl>();
  state->m_key = std::move(key);
  return std::make_unique<pem_rsa_key_wrapper>(std::move(state));
}

result<std::string> pem_rsa_key_wrapper::wrap(std::string_view aes_key) const {
  std::unique_ptr<EVP_PKEY_CTX, pkey_ctx_deleter> context{
      EVP_PKEY_CTX_new(m_impl->m_key.get(), nullptr)};
  if (!context || EVP_PKEY_encrypt_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_PADDING) != 1) {
    return std::unexpected(make_error(error_kind::internal, "openssl: cannot start RSA wrapping"));
  }

  auto const* input = reinterpret_cast<unsigned char const*>(aes_key.data());
  std::size_t length = 0;
  if (EVP_PKEY_encrypt(context.get(), nullptr, &length, input, aes_key.size()) != 1) {
    return std::unexpected(make_error(error_kind::internal, "openssl: cannot size RSA output"));
  }

  std::string wrapped(length, '\0');
  if (EVP_PKEY_encrypt(context.get(), reinterpret_cast<unsigned char*>(wrapped.data()), &length,
                       input, aes_key.size()) != 1) {
    return std::unexpected(make_error(error_kind::internal, "openssl: RSA wrapping failed"));
  }
  wrapped.resize(length);

  if (wrapped.size() != wrapped_key_length) {
    return std::unexpected(
        make_error(error_kind::internal, "openssl: unexpected RSA ciphertext length"));
  }

  return codec::base64_encode(wrapped);
}

namespace {

/// A URL is absolute when it is not blank and carries a `scheme://` prefix with a non-empty scheme.
/// The offending URL is never echoed back: it may carry a token in its query string.
std::optional<error> check_absolute_url(std::string_view url) {
  auto const text = strings::trim(url);
  if (text.empty()) {
    return make_error(error_kind::configuration, "cosy: the request URL must not be blank");
  }

  auto const separator = text.find("://");
  if (separator == std::string_view::npos || separator == 0) {
    return make_error(error_kind::configuration,
                      "cosy: the request URL is not an absolute URL (expected scheme://host/...)");
  }
  return std::nullopt;
}

} // namespace

std::string signed_path(std::string_view url) {
  if (strings::trim(url).empty()) {
    throw std::invalid_argument("cosy: the URL must not be blank");
  }

  auto const scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    throw std::invalid_argument("cosy: the URL must be absolute");
  }

  auto rest = url.substr(scheme + 3);
  auto const authority_end = rest.find_first_of("/?#");
  if (authority_end == std::string_view::npos) {
    return "/";
  }
  rest.remove_prefix(authority_end);

  auto path = rest.substr(0, rest.find_first_of("?#"));
  if (path.empty()) {
    return "/";
  }
  if (strings::starts_with(path, "/algo")) {
    path.remove_prefix(5);
  }
  return std::string{path};
}

std::string machine_os() {
  std::string text;
#if defined(__x86_64__) || defined(_M_X64)
  text = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  text = "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
  text = "x86";
#else
  text = "other";
#endif

  text.push_back('_');

#if defined(_WIN32)
  text += "win32";
#elif defined(__APPLE__)
  text += "darwin";
#elif defined(__linux__)
  text += "linux";
#elif defined(__FreeBSD__)
  text += "freebsd";
#else
  text += "unknown";
#endif

  return text;
}

result<std::string> encrypt_info(cosy_identity const& identity, std::string_view aes_key) {
  if (aes_key.size() != aes_key_length) {
    return std::unexpected(
        make_error(error_kind::protocol, "cosy: the AES key must be 16 ASCII characters"));
  }

  nlohmann::ordered_json info;
  info["uid"] = identity.m_uid;
  info["aid"] = "";
  info["name"] = identity.m_name;
  info["email"] = identity.m_email;
  info["security_oauth_token"] = identity.m_access_token;
  auto const plain = info.dump();

  std::unique_ptr<EVP_CIPHER_CTX, cipher_ctx_deleter> context{EVP_CIPHER_CTX_new()};
  if (!context) {
    return std::unexpected(
        make_error(error_kind::internal, "openssl: cannot create an AES context"));
  }

  auto const* key_bytes = reinterpret_cast<unsigned char const*>(aes_key.data());
  if (EVP_EncryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, key_bytes, key_bytes) != 1) {
    return std::unexpected(
        make_error(error_kind::internal, "openssl: cannot initialise AES-128-CBC"));
  }

  std::string ciphertext(plain.size() + EVP_MAX_BLOCK_LENGTH, '\0');
  int written = 0;
  if (EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()),
                        &written, reinterpret_cast<unsigned char const*>(plain.data()),
                        static_cast<int>(plain.size())) != 1) {
    return std::unexpected(make_error(error_kind::internal, "openssl: AES encryption failed"));
  }

  int padding = 0;
  if (EVP_EncryptFinal_ex(context.get(),
                          reinterpret_cast<unsigned char*>(ciphertext.data()) + written,
                          &padding) != 1) {
    return std::unexpected(make_error(error_kind::internal, "openssl: AES padding failed"));
  }
  ciphertext.resize(static_cast<std::size_t>(written + padding));

  return codec::base64_encode(ciphertext);
}

std::string build_signature_input(std::string_view payload, std::string_view wrapped_key,
                                  std::int64_t unix_seconds, std::string_view wire_body,
                                  std::string_view path) {
  std::string input;
  input.reserve(payload.size() + wrapped_key.size() + wire_body.size() + path.size() + 32);
  input.append(payload);
  input.push_back('\n');
  input.append(wrapped_key);
  input.push_back('\n');
  input.append(std::to_string(unix_seconds));
  input.push_back('\n');
  input.append(wire_body);
  input.push_back('\n');
  input.append(path);
  return input;
}

namespace {

result<cosy_material> build_cosy_material(std::string_view url, cosy_identity const& identity,
                                          std::string_view wire_body, std::int64_t unix_seconds,
                                          rsa_key_wrapper const& wrapper,
                                          std::optional<std::string> const& aes_key,
                                          std::optional<std::string> const& request_id) {
  if (auto problem = check_absolute_url(url)) {
    return std::unexpected(std::move(*problem));
  }

  if (strings::trim(identity.m_uid).empty() || strings::trim(identity.m_machine_id).empty()) {
    return std::unexpected(
        make_error(error_kind::configuration,
                   "The stored account has no uid or machine id; run 'qoderz login' again."));
  }

  std::string key;
  if (aes_key) {
    if (aes_key->size() != aes_key_length || !is_lower_hex(*aes_key)) {
      return std::unexpected(
          make_error(error_kind::configuration,
                     "cosy: the injected AES key must be 16 lowercase hex characters"));
    }
    key = *aes_key;
  } else {
    key = codec::random_hex(8);
  }

  auto info = encrypt_info(identity, key);
  if (!info) {
    return std::unexpected(info.error());
  }

  auto wrapped = wrapper.wrap(key);
  if (!wrapped) {
    return std::unexpected(wrapped.error());
  }

  auto const identifier = request_id ? *request_id : codec::random_hex(16);

  nlohmann::ordered_json envelope;
  envelope["version"] = "v1";
  envelope["requestId"] = identifier;
  envelope["info"] = *info;
  envelope["cosyVersion"] = cosy_version;
  envelope["ideVersion"] = "";
  auto const payload = codec::base64_encode(envelope.dump());

  auto const path = signed_path(url);
  auto const signature =
      codec::md5_hex(build_signature_input(payload, *wrapped, unix_seconds, wire_body, path));

  cosy_material material;
  material.m_aes_key = key;
  material.m_wrapped_key = *wrapped;
  material.m_info = *info;
  material.m_payload = payload;
  material.m_signature = signature;
  material.m_authorization = "Bearer COSY." + payload + "." + signature;
  material.m_signed_path = path;
  material.m_headers = {
      {"Accept", "application/json"},
      {"Accept-Encoding", "identity"},
      {"Content-Type", "application/json"},
      {"Authorization", material.m_authorization},
      {"Cosy-Business-Product", "app"},
      {"Cosy-Business-Type", "agent"},
      {"Cosy-ClientIp", identity.m_machine_id},
      {"Cosy-ClientType", "10"},
      {"Cosy-Data-Policy", "disagree"},
      {"Cosy-Date", std::to_string(unix_seconds)},
      {"Cosy-Key", material.m_wrapped_key},
      {"Cosy-MachineId", identity.m_machine_id},
      {"Cosy-MachineToken", identity.m_machine_id},
      {"Cosy-MachineType", "5"},
      {"Cosy-MachineOS", identity.m_machine_os},
      {"Cosy-Scene", "app"},
      {"Cosy-User", identity.m_uid},
      {"Cosy-Version", std::string{cosy_version}},
      {"Login-Version", "v2"},
  };

  return material;
}

} // namespace

result<cosy_material> create_cosy_headers(std::string_view url, cosy_identity const& identity,
                                          std::string_view wire_body, std::int64_t unix_seconds,
                                          rsa_key_wrapper const& wrapper,
                                          std::optional<std::string> const& aes_key,
                                          std::optional<std::string> const& request_id) {
  // Validate before the catch-all: a malformed URL is a configuration mistake, not an internal
  // signing failure, and must not be reported as one.
  if (auto problem = check_absolute_url(url)) {
    return std::unexpected(std::move(*problem));
  }

  try {
    return build_cosy_material(url, identity, wire_body, unix_seconds, wrapper, aes_key,
                               request_id);
  } catch (std::exception const&) {
    return std::unexpected(
        make_error(error_kind::internal, "cosy: signing failed; details are withheld"));
  } catch (...) {
    return std::unexpected(
        make_error(error_kind::internal, "cosy: signing failed; details are withheld"));
  }
}

} // namespace qoderz::protocol
