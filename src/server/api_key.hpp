#pragma once

#include "common/strings.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::server {

namespace detail {

/// Byte-wise comparison whose running time depends on the two lengths only, never on where the
/// first difference sits. A length mismatch still walks the whole loop, so the key length does not
/// leak through timing either.
[[nodiscard]] inline bool constant_time_equals(std::string_view presented,
                                               std::string_view expected) noexcept {
  auto const length = std::max(presented.size(), expected.size());
  auto difference = static_cast<unsigned char>(presented.size() == expected.size() ? 0U : 1U);
  for (std::size_t index = 0; index < length; ++index) {
    auto const left = index < presented.size() ? static_cast<unsigned char>(presented[index]) : 0U;
    auto const right = index < expected.size() ? static_cast<unsigned char>(expected[index]) : 0U;
    difference = static_cast<unsigned char>(difference | static_cast<unsigned char>(left ^ right));
  }
  return difference == 0;
}

} // namespace detail

/// Whether a request may pass, per docs/design/server.md §1.
///
/// Without a configured key every request passes; that is the localhost default, and the reason
/// `validate_config` refuses a public listener that has no key. With a key the header must start
/// with `Bearer ` (scheme case-insensitive, both sides trimmed) and the token must equal the key in
/// constant time. The header is never logged, and the comparison never short-circuits, so this is
/// safe on attacker-controlled input.
[[nodiscard]] inline bool authorize(std::optional<std::string> const& api_key,
                                    std::string_view authorization) noexcept {
  if (!api_key.has_value() || api_key->empty()) {
    return true;
  }
  auto const header = strings::trim(authorization);
  constexpr std::string_view scheme = "Bearer ";
  if (header.size() < scheme.size()) {
    return false;
  }
  if (!strings::equals_ignore_case(header.substr(0, scheme.size()), scheme)) {
    return false;
  }
  auto const presented = strings::trim(header.substr(scheme.size()));
  return detail::constant_time_equals(presented, *api_key);
}

} // namespace qoderz::server
