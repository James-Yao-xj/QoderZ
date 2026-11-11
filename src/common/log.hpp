#pragma once

#include "common/strings.hpp"

#include <initializer_list>
#include <optional>
#include <string_view>

namespace qoderz {

enum class log_level { trace, debug, info, warn, error, critical };

struct log_field {
  std::string_view m_key;
  std::string_view m_value;
};

using log_fields = std::initializer_list<log_field>;

constexpr std::string_view to_string(log_level level) noexcept {
  switch (level) {
  case log_level::trace:
    return "trace";
  case log_level::debug:
    return "debug";
  case log_level::info:
    return "info";
  case log_level::warn:
    return "warn";
  case log_level::error:
    return "error";
  case log_level::critical:
    return "critical";
  }
  return "unknown";
}

constexpr std::optional<log_level> parse_log_level(std::string_view text) noexcept {
  if (strings::equals_ignore_case(text, "trace")) {
    return log_level::trace;
  }
  if (strings::equals_ignore_case(text, "debug")) {
    return log_level::debug;
  }
  if (strings::equals_ignore_case(text, "info")) {
    return log_level::info;
  }
  if (strings::equals_ignore_case(text, "warn")) {
    return log_level::warn;
  }
  if (strings::equals_ignore_case(text, "error")) {
    return log_level::error;
  }
  if (strings::equals_ignore_case(text, "critical")) {
    return log_level::critical;
  }
  return std::nullopt;
}

void set_log_level(log_level level) noexcept;

[[nodiscard]] log_level current_log_level() noexcept;

void log(log_level level, std::string_view message, log_fields fields = {});

} // namespace qoderz
