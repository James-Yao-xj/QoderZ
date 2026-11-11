#pragma once

#include <cstddef>
#include <string>

namespace qoderz::codec {

/// Cryptographically secure random bytes (`RAND_bytes`). Throws `std::runtime_error` when the
/// generator fails, because signing without fresh randomness is not recoverable.
[[nodiscard]] std::string random_bytes(std::size_t count);

/// `2 * bytes` lowercase hex characters.
[[nodiscard]] std::string random_hex(std::size_t bytes);

/// base64url without padding; length is `ceil(bytes / 3) * 4` minus padding.
[[nodiscard]] std::string random_base64url(std::size_t bytes);

} // namespace qoderz::codec
