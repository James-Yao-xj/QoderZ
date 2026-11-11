#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace qoderz::protocol {

inline constexpr std::string_view body_alphabet =
    "_doRTgHZBKcGVjlvpC,@aFSx#DPuNJme&i*MzLOEn)sUrthbf%Y^w.(kIQyXqWA!";
inline constexpr char body_padding = '$';

static_assert(body_alphabet.size() == 64);

/// Four characters per three bytes, rounded up to a whole group.
[[nodiscard]] constexpr std::size_t max_encoded_length(std::size_t plain_size) noexcept {
  return ((plain_size + 2) / 3) * 4;
}

/// Three bytes per four characters; the trailing partial group is not counted.
[[nodiscard]] constexpr std::size_t max_decoded_length(std::size_t encoded_size) noexcept {
  return (encoded_size / 4) * 3;
}

/// Writes `max_encoded_length(plain.size())` characters and returns that count.
///
/// Contract when `destination` is too small for the encoded form: nothing is written, the
/// destination is left untouched, and the function returns `0`. A zero return therefore always
/// means "nothing encoded", never "encoded the empty input" (empty input also writes nothing, so
/// still returns `0`); callers that need to tell the two apart check `plain.empty()`. Size the
/// buffer with `max_encoded_length(plain.size())` and compare the return value with it.
[[nodiscard]] std::size_t body_encode(std::string_view plain, std::span<char> destination);

[[nodiscard]] std::string body_encode(std::string_view plain);
/// Consumes whole four-character groups only; a trailing partial group is ignored. `$` is skipped,
/// any character outside `body_alphabet` is `error_kind::protocol`.
[[nodiscard]] result<std::string> body_decode(std::string_view encoded);

} // namespace qoderz::protocol
