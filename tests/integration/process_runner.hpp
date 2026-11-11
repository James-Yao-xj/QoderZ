#pragma once

#include "common/error.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace qoderz::test {

enum class process_stream { out, err };

/// A child process started without a shell.
///
/// stdin comes from `/dev/null`, stdout and stderr are drained by one reader thread, so a chatty
/// child can never block on a full pipe and every line stays available for later assertions. The
/// executable must be an absolute path, so a test can never pick up a different binary from `PATH`.
class process_runner {
public:
  struct options {
    std::filesystem::path m_executable;
    std::vector<std::string> m_arguments;
    std::map<std::string, std::string> m_environment;
  };

  /// Spawns the child; a failure to start is `error_kind::io` and carries `strerror`.
  [[nodiscard]] static qoderz::result<process_runner> start(options const& options);

  process_runner(process_runner&& other) noexcept;
  process_runner& operator=(process_runner&& other) noexcept;
  process_runner(process_runner const&) = delete;
  process_runner& operator=(process_runner const&) = delete;

  /// Kills a child that was not waited for, so a failing test cannot leak a process.
  ~process_runner();

  [[nodiscard]] pid_t pid() const noexcept;

  /// Waits for a complete line containing `needle` on one of the two streams.
  [[nodiscard]] std::optional<std::string>
  wait_for_line(process_stream which, std::string_view needle, std::chrono::milliseconds timeout);

  [[nodiscard]] std::string captured(process_stream which) const;

  /// SIGTERM, then the child's exit code; `std::nullopt` when it outlived `grace`.
  [[nodiscard]] std::optional<int> terminate(std::chrono::milliseconds grace);

  [[nodiscard]] std::optional<int> wait_for_exit(std::chrono::milliseconds timeout);

  void kill();

  struct state;

private:
  explicit process_runner(std::shared_ptr<state> shared, pid_t pid) noexcept;

  void release();

  [[nodiscard]] bool reap();
  void reap_blocking();
  void join_reader();

  std::shared_ptr<state> m_state;
  pid_t m_pid = -1;
  std::thread m_reader;
};

} // namespace qoderz::test
