#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::protocol {

/// Splits a byte stream into lines on `\n`, stripping one trailing `\r` per line. Empty lines are
/// delivered, and `finish` delivers a final line that never saw a `\n`. The reader does not
/// interpret `data:`; it only splits.
///
/// A line above `max_line_bytes` is `error_kind::protocol`; from then on every `feed`/`finish`
/// returns the same error until `reset`. The limit applies to the delivered line, so an
/// un-terminated line of exactly `max_line_bytes` bytes plus a trailing `\r` is still accepted. Not
/// thread-safe.
class sse_line_reader {
public:
  static constexpr std::size_t max_line_bytes = 16 * 1024 * 1024;

  [[nodiscard]] result<std::vector<std::string>> feed(std::string_view bytes);
  [[nodiscard]] result<std::vector<std::string>> finish();
  void reset();

private:
  [[nodiscard]] error line_too_long() const;
  [[nodiscard]] result<std::vector<std::string>> fail(error failure);

  std::string m_buffer;
  std::optional<error> m_failure;
};

} // namespace qoderz::protocol
