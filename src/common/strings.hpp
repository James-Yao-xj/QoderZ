#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace qoderz::strings {

/// ASCII whitespace only (space, \t, \n, \v, \f, \r); protocol fields are ASCII, so Unicode
/// whitespace is deliberately not treated as blank.
constexpr bool is_ascii_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\v' || value == '\f' ||
         value == '\r';
}

constexpr char ascii_lower(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

constexpr bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) {
      return false;
    }
  }
  return true;
}

constexpr bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

constexpr std::string_view trim_start(std::string_view text) noexcept {
  while (!text.empty() && is_ascii_space(text.front())) {
    text.remove_prefix(1);
  }
  return text;
}

constexpr std::string_view trim_end(std::string_view text) noexcept {
  while (!text.empty() && is_ascii_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

constexpr std::string_view trim(std::string_view text) noexcept {
  return trim_end(trim_start(text));
}
constexpr bool ends_with(std::string_view text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// Lowercase ASCII copy; non-ASCII bytes are left untouched, matching `ascii_lower`.
[[nodiscard]] inline std::string ascii_lower_copy(std::string_view text) {
  std::string lowered{text};
  for (auto& value : lowered) {
    value = ascii_lower(value);
  }
  return lowered;
}

} // namespace qoderz::strings
