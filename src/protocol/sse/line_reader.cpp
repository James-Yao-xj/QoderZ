#include "protocol/sse/line_reader.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::protocol {
namespace {

constexpr std::string_view carry_return = "\r";

bool ends_with_carriage_return(std::string_view line) noexcept {
  return !line.empty() && line.back() == carry_return.front();
}

} // namespace

error sse_line_reader::line_too_long() const {
  return make_error(error_kind::protocol, "the Qoder stream sent a line longer than " +
                                              std::to_string(max_line_bytes) + " bytes");
}

result<std::vector<std::string>> sse_line_reader::fail(error failure) {
  m_failure = failure;
  return std::unexpected(std::move(failure));
}

result<std::vector<std::string>> sse_line_reader::feed(std::string_view bytes) {
  if (m_failure) {
    return std::unexpected(*m_failure);
  }

  m_buffer.append(bytes);

  std::vector<std::string> lines;
  std::size_t start = 0;
  while (true) {
    auto const newline = m_buffer.find('\n', start);
    if (newline == std::string::npos) {
      break;
    }

    auto line = std::string_view{m_buffer}.substr(start, newline - start);
    if (ends_with_carriage_return(line)) {
      line.remove_suffix(1);
    }
    if (line.size() > max_line_bytes) {
      return fail(line_too_long());
    }
    lines.emplace_back(line);
    start = newline + 1;
  }

  m_buffer.erase(0, start);

  if (m_buffer.size() > max_line_bytes + 1) {
    return fail(line_too_long());
  }

  return lines;
}

result<std::vector<std::string>> sse_line_reader::finish() {
  if (m_failure) {
    return std::unexpected(*m_failure);
  }

  std::vector<std::string> lines;
  if (!m_buffer.empty()) {
    auto line = std::string_view{m_buffer};
    if (ends_with_carriage_return(line)) {
      line.remove_suffix(1);
    }
    // Measured after the `\r` is stripped, so a line of exactly `max_line_bytes` followed by a
    // carriage return is accepted: the byte count that matters is the delivered line.
    if (line.size() > max_line_bytes) {
      return fail(line_too_long());
    }
    lines.emplace_back(line);
    m_buffer.clear();
  }
  return lines;
}

void sse_line_reader::reset() {
  m_buffer.clear();
  m_failure.reset();
}

} // namespace qoderz::protocol
