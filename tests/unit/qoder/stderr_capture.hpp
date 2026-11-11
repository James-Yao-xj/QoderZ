#pragma once

#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace qoderz::test {

/// Collects everything written to stderr while it is alive.
///
/// Only the file descriptor is swapped, so `std::fwrite(stderr)` from the logger lands in the
/// capture and a test can assert on the text instead of reading test output by eye.
class stderr_capture {
public:
  stderr_capture() {
#if !defined(_WIN32)
    std::fflush(stderr);
    m_file = std::tmpfile();
    if (m_file == nullptr) {
      return;
    }
    m_saved = ::dup(fileno(stderr));
    if (m_saved < 0) {
      std::fclose(m_file);
      m_file = nullptr;
      return;
    }
    ::dup2(fileno(m_file), fileno(stderr));
#endif
  }

  ~stderr_capture() { (void)release(); }

  stderr_capture(stderr_capture const&) = delete;
  stderr_capture& operator=(stderr_capture const&) = delete;

  [[nodiscard]] bool available() const noexcept { return m_file != nullptr; }

  /// Flushes, restores stderr and returns what was captured; a second call returns an empty string.
  std::string release() {
    std::string text;
#if !defined(_WIN32)
    if (m_file == nullptr) {
      return text;
    }
    std::fflush(stderr);
    ::dup2(m_saved, fileno(stderr));
    ::close(m_saved);
    m_saved = -1;
    std::rewind(m_file);
    char buffer[4096];
    while (auto const read = std::fread(buffer, 1, sizeof(buffer), m_file)) {
      text.append(buffer, read);
    }
    std::fclose(m_file);
    m_file = nullptr;
#endif
    return text;
  }

private:
  int m_saved = -1;
  std::FILE* m_file = nullptr;
};

} // namespace qoderz::test
