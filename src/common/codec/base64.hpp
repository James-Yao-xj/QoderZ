#pragma once

#include "common/error.hpp"

#include <span>
#include <string>
#include <string_view>

namespace qoderz::codec {

inline constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view base64url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
inline constexpr char base64_padding = '=';

static_assert(base64_alphabet.size() == 64);
static_assert(base64url_alphabet.size() == 64);

/// Standard base64 with `=` padding.
[[nodiscard]] std::string base64_encode(std::span<std::byte const> data);
[[nodiscard]] std::string base64_encode(std::string_view data);

/// base64url (`-_` alphabet). Unpadded by default; `padding` restores `=`.
[[nodiscard]] std::string base64url_encode(std::span<std::byte const> data, bool padding = false);

/// Decoding accepts padded and unpadded input; anything outside the alphabet, a stray `=`, or an
/// impossible length is `error_kind::protocol`.
[[nodiscard]] result<std::string> base64_decode(std::string_view text);
[[nodiscard]] result<std::string> base64url_decode(std::string_view text);

} // namespace qoderz::codec
