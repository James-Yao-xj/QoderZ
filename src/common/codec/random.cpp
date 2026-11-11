#include "common/codec/random.hpp"

#include "common/codec/base64.hpp"
#include "common/codec/hash.hpp"

#include <cstddef>
#include <limits>
#include <openssl/rand.h>
#include <span>
#include <stdexcept>
#include <string>

namespace qoderz::codec {

std::string random_bytes(std::size_t count) {
  if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("openssl: random request is too large");
  }
  std::string bytes(count, '\0');
  if (count == 0) {
    return bytes;
  }
  if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(count)) != 1) {
    throw std::runtime_error("openssl: RAND_bytes failed");
  }
  return bytes;
}

std::string random_hex(std::size_t bytes) {
  auto const data = random_bytes(bytes);
  return hex_encode(std::as_bytes(std::span{data.data(), data.size()}));
}

std::string random_base64url(std::size_t bytes) {
  auto const data = random_bytes(bytes);
  return base64url_encode(std::as_bytes(std::span{data.data(), data.size()}), false);
}

} // namespace qoderz::codec
