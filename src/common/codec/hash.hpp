#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace qoderz::codec {

/// Incremental SHA-256. `hex_digest` finalises the hash; calling it twice returns the same cached
/// hex string, and updates after the first digest are ignored.
class sha256 {
public:
  sha256();
  ~sha256();
  sha256(sha256&&) noexcept;
  sha256& operator=(sha256&&) noexcept;
  sha256(sha256 const&) = delete;
  sha256& operator=(sha256 const&) = delete;

  void update(std::span<std::byte const> data);
  void update(std::string_view data);
  [[nodiscard]] std::string hex_digest();

private:
  struct impl;
  std::unique_ptr<impl> m_impl;
};

[[nodiscard]] std::string sha256_hex(std::span<std::byte const> data);
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] std::string md5_hex(std::span<std::byte const> data);
[[nodiscard]] std::string md5_hex(std::string_view data);

/// Lowercase hex, two characters per byte.
[[nodiscard]] std::string hex_encode(std::span<std::byte const> data);

} // namespace qoderz::codec
