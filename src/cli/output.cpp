#include "cli/output.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace qoderz::cli {
namespace {

constexpr char escape = '\x1b';

constexpr std::string_view code_of(tone shade) noexcept {
  switch (shade) {
  case tone::red:
    return "31";
  case tone::green:
    return "32";
  case tone::yellow:
    return "33";
  case tone::cyan:
    return "36";
  case tone::bold:
    return "1";
  }
  return "0";
}

constexpr std::string_view empty_cell = "-";

struct local_parts {
  std::tm m_tm{};
  bool m_ok = false;
};

local_parts to_local(std::int64_t unix_ms) {
  local_parts parts;
  if (unix_ms <= 0) {
    return parts;
  }
  std::time_t const seconds = static_cast<std::time_t>(unix_ms / 1000);
  if (localtime_r(&seconds, &parts.m_tm) == nullptr) {
    return parts;
  }
  parts.m_ok = true;
  return parts;
}

/// `±HH:MM`; `localtime_r` fills `%z` as `+0800`, so the colon is inserted rather than assumed.
std::string offset_of(std::tm const& value) {
  std::array<char, 16> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%z", &value) == 0) {
    return {};
  }
  std::string offset{buffer.data()};
  if (offset.size() == 5 && (offset.front() == '+' || offset.front() == '-')) {
    offset.insert(3, ":");
  }
  return offset;
}

std::string strftime_part(std::tm const& value, char const* format) {
  std::array<char, 32> buffer{};
  auto const written = std::strftime(buffer.data(), buffer.size(), format, &value);
  return std::string(buffer.data(), written);
}

} // namespace

std::string paint(tone shade, std::string_view text, bool enabled) {
  if (!enabled) {
    return std::string{text};
  }
  std::string painted;
  painted.reserve(text.size() + 8);
  painted.push_back(escape);
  painted.push_back('[');
  painted.append(code_of(shade));
  painted.push_back('m');
  painted.append(text);
  painted.push_back(escape);
  painted.push_back('[');
  painted.push_back('0');
  painted.push_back('m');
  return painted;
}

std::size_t visible_length(std::string_view text) noexcept {
  std::size_t visible = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == escape && index + 1 < text.size() && text[index + 1] == '[') {
      index += 2;
      while (index < text.size() && text[index] != 'm') {
        ++index;
      }
      continue;
    }
    ++visible;
  }
  return visible;
}

std::string render(table const& value) {
  std::vector<std::size_t> widths;
  widths.reserve(value.m_columns.size());
  for (auto const& column : value.m_columns) {
    widths.push_back(visible_length(column.m_header));
  }
  for (auto const& row : value.m_rows) {
    for (std::size_t index = 0; index < widths.size() && index < row.size(); ++index) {
      widths[index] = std::max(widths[index], visible_length(row[index]));
    }
  }

  std::string text;
  auto const append_row = [&](std::vector<std::string> const& cells) {
    for (std::size_t index = 0; index < widths.size(); ++index) {
      std::string cell = index < cells.size() ? cells[index] : std::string{};
      if (cell.empty()) {
        cell = empty_cell;
      }
      auto const padding = widths[index] - visible_length(cell);
      if (index != 0) {
        text.append("  ");
      }
      bool const right = value.m_columns[index].m_right;
      if (right) {
        text.append(padding, ' ');
      }
      text.append(cell);
      if (!right) {
        text.append(padding, ' ');
      }
    }
    // Trailing padding of the last column carries no meaning on a terminal.
    auto const trimmed = text.find_last_not_of(' ');
    text.erase(trimmed == std::string::npos ? 0 : trimmed + 1);
    text.push_back('\n');
  };

  std::vector<std::string> headers;
  headers.reserve(value.m_columns.size());
  for (auto const& column : value.m_columns) {
    headers.push_back(column.m_header);
  }
  append_row(headers);
  for (auto const& row : value.m_rows) {
    append_row(row);
  }
  return text;
}

std::string format_local_time(std::optional<std::int64_t> unix_ms) {
  if (!unix_ms.has_value()) {
    return std::string{empty_cell};
  }
  auto const parts = to_local(*unix_ms);
  if (!parts.m_ok) {
    return std::string{empty_cell};
  }
  std::string text = strftime_part(parts.m_tm, "%Y-%m-%d %H:%M");
  auto const offset = offset_of(parts.m_tm);
  if (!offset.empty()) {
    text.push_back(' ');
    text.append(offset);
  }
  return text;
}

std::optional<std::string> format_iso8601(std::optional<std::int64_t> unix_ms) {
  if (!unix_ms.has_value()) {
    return std::nullopt;
  }
  auto const parts = to_local(*unix_ms);
  if (!parts.m_ok) {
    return std::nullopt;
  }
  std::string text = strftime_part(parts.m_tm, "%Y-%m-%dT%H:%M:%S");
  text.append(offset_of(parts.m_tm));
  return text;
}

std::string format_context(int tokens) {
  if (tokens <= 0) {
    return "0";
  }
  constexpr int million = 1'000'000;
  constexpr int thousand = 1'000;
  auto const format = [](int value, int unit, char suffix) {
    std::string text = std::to_string(value / unit);
    auto const remainder = value % unit;
    if (remainder != 0) {
      // One decimal, with the trailing zero dropped: 1_500_000 is `1.5M`, 1_200_000 is `1.2M`.
      auto const tenths = remainder * 10 / unit;
      if (tenths != 0) {
        text.push_back('.');
        text.push_back(static_cast<char>('0' + tenths));
      }
    }
    text.push_back(suffix);
    return text;
  };
  if (tokens >= million) {
    return format(tokens, million, 'M');
  }
  if (tokens >= thousand) {
    return format(tokens, thousand, 'K');
  }
  return std::to_string(tokens);
}

std::string yes_no(bool value) {
  return value ? "yes" : "no";
}

void write_json(std::ostream& out, json const& value) {
  out << value.dump() << '\n';
  out.flush();
}

} // namespace qoderz::cli
