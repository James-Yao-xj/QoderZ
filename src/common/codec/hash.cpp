#include "common/codec/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <openssl/evp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace qoderz::codec {
namespace {

struct ctx_deleter {
  void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

using ctx_ptr = std::unique_ptr<EVP_MD_CTX, ctx_deleter>;

ctx_ptr make_context(EVP_MD const* digest) {
  ctx_ptr context{EVP_MD_CTX_new()};
  if (!context || EVP_DigestInit_ex(context.get(), digest, nullptr) != 1) {
    throw std::runtime_error("openssl: cannot initialise a digest context");
  }
  return context;
}

void discard_or_throw(EVP_MD_CTX* context) {
  if (!context) {
    throw std::runtime_error("openssl: digest context is null");
  }
}

std::string digest_hex(EVP_MD const* algorithm, std::span<std::byte const> data) {
  auto context = make_context(algorithm);
  if (!data.empty() && EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1) {
    throw std::runtime_error("openssl: digest update failed");
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> buffer{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context.get(), buffer.data(), &length) != 1) {
    throw std::runtime_error("openssl: digest finalisation failed");
  }
  return hex_encode(std::as_bytes(std::span{buffer.data(), static_cast<std::size_t>(length)}));
}

} // namespace

struct sha256::impl {
  ctx_ptr m_context;
  std::optional<std::string> m_digest;
};

sha256::sha256() : m_impl{std::make_unique<impl>()} {
  m_impl->m_context = make_context(EVP_sha256());
}

sha256::~sha256() = default;
sha256::sha256(sha256&&) noexcept = default;
sha256& sha256::operator=(sha256&&) noexcept = default;

void sha256::update(std::span<std::byte const> data) {
  if (m_impl->m_digest || data.empty()) {
    return;
  }
  if (EVP_DigestUpdate(m_impl->m_context.get(), data.data(), data.size()) != 1) {
    throw std::runtime_error("openssl: digest update failed");
  }
}

void sha256::update(std::string_view data) {
  update(std::as_bytes(std::span{data}));
}

std::string sha256::hex_digest() {
  if (!m_impl->m_digest) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buffer{};
    unsigned int length = 0;
    discard_or_throw(m_impl->m_context.get());
    if (EVP_DigestFinal_ex(m_impl->m_context.get(), buffer.data(), &length) != 1) {
      throw std::runtime_error("openssl: digest finalisation failed");
    }
    m_impl->m_digest =
        hex_encode(std::as_bytes(std::span{buffer.data(), static_cast<std::size_t>(length)}));
  }
  return *m_impl->m_digest;
}

std::string sha256_hex(std::span<std::byte const> data) {
  return digest_hex(EVP_sha256(), data);
}

std::string sha256_hex(std::string_view data) {
  return sha256_hex(std::as_bytes(std::span{data}));
}

std::string md5_hex(std::span<std::byte const> data) {
  return digest_hex(EVP_md5(), data);
}

std::string md5_hex(std::string_view data) {
  return md5_hex(std::as_bytes(std::span{data}));
}

std::string hex_encode(std::span<std::byte const> data) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(data.size() * 2);
  for (auto const byte : data) {
    auto const value = std::to_integer<unsigned>(byte);
    text.push_back(digits[value >> 4U]);
    text.push_back(digits[value & 0x0FU]);
  }
  return text;
}

} // namespace qoderz::codec
