#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::protocol {

inline constexpr std::string_view cosy_version = "1.1.69";

struct cosy_identity {
  std::string m_uid;
  std::string m_name;
  std::string m_email;
  std::string m_access_token;
  std::string m_machine_id;
  std::string m_machine_os;
};

struct cosy_header {
  std::string m_name;
  std::string m_value;
};

using cosy_headers = std::vector<cosy_header>;

/// Encrypts the 16-character AES key with the embedded Qoder RSA key. Injected so tests can pin
/// the wrapped value; production uses `pem_rsa_key_wrapper`.
class rsa_key_wrapper {
public:
  rsa_key_wrapper() = default;
  virtual ~rsa_key_wrapper() = default;
  rsa_key_wrapper(rsa_key_wrapper const&) = delete;
  rsa_key_wrapper& operator=(rsa_key_wrapper const&) = delete;
  rsa_key_wrapper(rsa_key_wrapper&&) = delete;
  rsa_key_wrapper& operator=(rsa_key_wrapper&&) = delete;

  /// base64 of the RSA PKCS#1 v1.5 ciphertext, 128 bytes before encoding.
  [[nodiscard]] virtual result<std::string> wrap(std::string_view aes_key) const = 0;
};

class pem_rsa_key_wrapper final : public rsa_key_wrapper {
  struct impl;

public:
  ~pem_rsa_key_wrapper() override;

  [[nodiscard]] static result<std::unique_ptr<pem_rsa_key_wrapper>> create();

  /// Implementation detail: `create()` adopts the key loaded from the embedded PEM. The argument
  /// type is the private nested `impl`, so only this class can call the constructor.
  explicit pem_rsa_key_wrapper(std::unique_ptr<impl> state);

  [[nodiscard]] result<std::string> wrap(std::string_view aes_key) const override;

private:
  std::unique_ptr<impl> m_impl;
};

/// Absolute path of `url` with the leading `/algo` removed and query/fragment dropped; `/` when the
/// URL has no path. Throws `std::invalid_argument` for a blank URL or a URL without `scheme://`.
[[nodiscard]] std::string signed_path(std::string_view url);

/// `{arch}_{platform}`, for example `x86_64_linux`.
[[nodiscard]] std::string machine_os();

/// AES-128-CBC with PKCS7 padding and IV = key, base64 encoded. `aes_key` must be 16 characters.
[[nodiscard]] result<std::string> encrypt_info(cosy_identity const& identity,
                                               std::string_view aes_key);

/// `payload + "\n" + wrapped_key + "\n" + unix_seconds + "\n" + wire_body + "\n" + path`.
[[nodiscard]] std::string build_signature_input(std::string_view payload,
                                                std::string_view wrapped_key,
                                                std::int64_t unix_seconds,
                                                std::string_view wire_body, std::string_view path);

struct cosy_material {
  std::string m_aes_key;
  std::string m_wrapped_key;
  std::string m_info;
  std::string m_payload;
  std::string m_signature;
  std::string m_authorization;
  std::string m_signed_path;
  cosy_headers m_headers;
};

/// Builds the 19 COSY headers. `aes_key` and `request_id` exist for deterministic tests; when they
/// are absent fresh random values are generated. Missing uid or machine id is
/// `error_kind::configuration`.
[[nodiscard]] result<cosy_material>
create_cosy_headers(std::string_view url, cosy_identity const& identity, std::string_view wire_body,
                    std::int64_t unix_seconds, rsa_key_wrapper const& wrapper,
                    std::optional<std::string> const& aes_key = std::nullopt,
                    std::optional<std::string> const& request_id = std::nullopt);

} // namespace qoderz::protocol
