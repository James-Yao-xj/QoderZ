#pragma once

#include "common/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::cli {

enum class tone { red, green, yellow, cyan, bold };

/// Wraps `text` in SGR sequences when `enabled`; disabled means the text comes back unchanged.
[[nodiscard]] std::string paint(tone shade, std::string_view text, bool enabled);

/// Width a terminal shows, with SGR sequences skipped so a painted cell aligns with a plain one.
[[nodiscard]] std::size_t visible_length(std::string_view text) noexcept;

struct table_column {
  std::string m_header;
  bool m_right = false;
};

/// Columns padded to their widest cell, two spaces between them, right aligned when `m_right` is
/// set, and an empty cell printed as `-`.
struct table {
  std::vector<table_column> m_columns;
  std::vector<std::vector<std::string>> m_rows;
};

[[nodiscard]] std::string render(table const& value);

/// `YYYY-MM-DD HH:MM ±HH:MM` in the local time zone; an absent or non-positive instant is `-`.
[[nodiscard]] std::string format_local_time(std::optional<std::int64_t> unix_ms);

/// `YYYY-MM-DDTHH:MM:SS±HH:MM`, the JSON form of the same instant.
[[nodiscard]] std::optional<std::string> format_iso8601(std::optional<std::int64_t> unix_ms);

/// `1M`, `200K` or `0`.
[[nodiscard]] std::string format_context(int tokens);

[[nodiscard]] std::string yes_no(bool value);

/// Compact JSON plus one newline; `--json` output is exactly this and nothing else.
void write_json(std::ostream& out, json const& value);

} // namespace qoderz::cli
