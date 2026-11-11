#include "integration/process_runner.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace qoderz::test {
namespace {

using namespace std::chrono_literals;

constexpr auto poll_step = 5ms;

std::vector<std::string> merged_environment(std::map<std::string, std::string> const& overrides) {
  std::map<std::string, std::string> merged;
  if (environ != nullptr) {
    for (char** entry = environ; *entry != nullptr; ++entry) {
      std::string pair{*entry};
      auto const separator = pair.find('=');
      if (separator == std::string::npos) {
        continue;
      }
      merged[pair.substr(0, separator)] = pair.substr(separator + 1);
    }
  }
  for (auto const& [name, value] : overrides) {
    merged[name] = value;
  }

  std::vector<std::string> environment;
  environment.reserve(merged.size());
  for (auto const& [name, value] : merged) {
    environment.push_back(name + "=" + value);
  }
  return environment;
}

std::optional<std::string> find_line(std::string const& text, std::string_view needle,
                                     bool include_tail) {
  std::size_t start = 0;
  while (start < text.size()) {
    auto const end = text.find('\n', start);
    if (end == std::string::npos) {
      if (!include_tail) {
        return std::nullopt;
      }
      std::string_view const tail{text.data() + start, text.size() - start};
      if (tail.find(needle) == std::string_view::npos) {
        return std::nullopt;
      }
      return std::string{tail};
    }
    std::string_view const line{text.data() + start, end - start};
    if (line.find(needle) != std::string_view::npos) {
      std::string found{line};
      if (!found.empty() && found.back() == '\r') {
        found.pop_back();
      }
      return found;
    }
    start = end + 1;
  }
  return std::nullopt;
}

} // namespace

struct process_runner::state {
  mutable std::mutex m_mutex;
  std::condition_variable m_changed;
  std::string m_out;
  std::string m_err;
  bool m_out_open = true;
  bool m_err_open = true;
  std::atomic<bool> m_reaped{false};
  std::atomic<int> m_exit_code{-1};
};

namespace {

void pump(std::shared_ptr<process_runner::state> state, int out_fd, int err_fd) {
  pollfd entries[2] = {{.fd = out_fd, .events = POLLIN, .revents = 0},
                       {.fd = err_fd, .events = POLLIN, .revents = 0}};

  for (;;) {
    bool const none_left = entries[0].fd < 0 && entries[1].fd < 0;
    if (none_left) {
      break;
    }
    auto const ready = ::poll(entries, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (std::size_t index = 0; index < 2; ++index) {
      auto& entry = entries[index];
      if (entry.fd < 0 || entry.revents == 0) {
        continue;
      }
      char buffer[4096];
      auto const got = ::read(entry.fd, buffer, sizeof(buffer));
      if (got > 0) {
        std::lock_guard const lock{state->m_mutex};
        (index == 0 ? state->m_out : state->m_err).append(buffer, static_cast<std::size_t>(got));
        state->m_changed.notify_all();
        continue;
      }
      if (got < 0 && errno == EINTR) {
        continue;
      }
      ::close(entry.fd);
      entry.fd = -1;
      entry.events = 0;
      std::lock_guard const lock{state->m_mutex};
      (index == 0 ? state->m_out_open : state->m_err_open) = false;
      state->m_changed.notify_all();
    }
  }
}

} // namespace

qoderz::result<process_runner> process_runner::start(options const& options) {
  if (options.m_executable.empty() || !options.m_executable.is_absolute()) {
    return std::unexpected(
        make_error(error_kind::io, "the test child process needs an absolute executable path"));
  }
  std::error_code exists_error;
  if (!std::filesystem::exists(options.m_executable, exists_error)) {
    return std::unexpected(make_error(error_kind::io, "the test child executable does not exist: " +
                                                          options.m_executable.string()));
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
    return std::unexpected(make_error(error_kind::io, "the test child pipe could not be created: " +
                                                          std::string{std::strerror(errno)}));
  }
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    int const failure = errno;
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return std::unexpected(make_error(error_kind::io, "the test child pipe could not be created: " +
                                                          std::string{std::strerror(failure)}));
  }

  std::vector<std::string> const environment = merged_environment(options.m_environment);
  std::vector<std::string> const arguments = [&options] {
    std::vector<std::string> built;
    built.reserve(options.m_arguments.size() + 1);
    built.push_back(options.m_executable.string());
    built.insert(built.end(), options.m_arguments.begin(), options.m_arguments.end());
    return built;
  }();

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto const& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto const& entry : environment) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

  pid_t pid = -1;
  auto const spawned = ::posix_spawn(&pid, options.m_executable.c_str(), &actions, nullptr,
                                     argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);

  if (spawned != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    return std::unexpected(make_error(error_kind::io, "the test child process could not start: " +
                                                          std::string{std::strerror(spawned)}));
  }

  auto connection_state = std::make_shared<process_runner::state>();
  process_runner runner{connection_state, pid};
  runner.m_reader = std::thread{pump, std::move(connection_state), out_pipe[0], err_pipe[0]};
  return runner;
}

process_runner::process_runner(std::shared_ptr<state> shared, pid_t pid) noexcept
    : m_state{std::move(shared)}, m_pid{pid} {}

process_runner::process_runner(process_runner&& other) noexcept
    : m_state{std::move(other.m_state)}, m_pid{other.m_pid}, m_reader{std::move(other.m_reader)} {
  other.m_pid = -1;
}

process_runner& process_runner::operator=(process_runner&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  m_state = std::move(other.m_state);
  m_pid = other.m_pid;
  m_reader = std::move(other.m_reader);
  other.m_pid = -1;
  return *this;
}

process_runner::~process_runner() {
  release();
}

void process_runner::release() {
  if (m_state == nullptr) {
    return;
  }
  if (m_pid > 0 && !reap()) {
    ::kill(m_pid, SIGKILL);
    reap_blocking();
  }
  join_reader();
  m_state.reset();
  m_pid = -1;
}

pid_t process_runner::pid() const noexcept {
  return m_pid;
}

bool process_runner::reap() {
  if (m_state->m_reaped.load()) {
    return true;
  }
  int status = 0;
  auto const got = ::waitpid(m_pid, &status, WNOHANG);
  if (got == 0) {
    return false;
  }
  if (got == m_pid) {
    if (WIFEXITED(status)) {
      m_state->m_exit_code.store(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      m_state->m_exit_code.store(128 + WTERMSIG(status));
    }
  }
  m_state->m_reaped.store(true);
  m_state->m_changed.notify_all();
  return true;
}

void process_runner::reap_blocking() {
  if (m_state->m_reaped.load()) {
    return;
  }
  int status = 0;
  auto const got = ::waitpid(m_pid, &status, 0);
  if (got == m_pid) {
    if (WIFEXITED(status)) {
      m_state->m_exit_code.store(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      m_state->m_exit_code.store(128 + WTERMSIG(status));
    }
  }
  m_state->m_reaped.store(true);
  m_state->m_changed.notify_all();
}

void process_runner::join_reader() {
  if (m_reader.joinable()) {
    m_reader.join();
  }
}

std::optional<std::string> process_runner::wait_for_line(process_stream which,
                                                         std::string_view needle,
                                                         std::chrono::milliseconds timeout) {
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock{m_state->m_mutex};
  for (;;) {
    std::string const& text = which == process_stream::out ? m_state->m_out : m_state->m_err;
    bool const closed = which == process_stream::out ? !m_state->m_out_open : !m_state->m_err_open;
    if (auto found = find_line(text, needle, closed); found.has_value()) {
      return found;
    }
    if (closed) {
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    m_state->m_changed.wait_until(lock, deadline);
  }
}

std::string process_runner::captured(process_stream which) const {
  std::lock_guard const lock{m_state->m_mutex};
  return which == process_stream::out ? m_state->m_out : m_state->m_err;
}

std::optional<int> process_runner::wait_for_exit(std::chrono::milliseconds timeout) {
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (reap()) {
      join_reader();
      return m_state->m_exit_code.load();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(poll_step);
  }
}

std::optional<int> process_runner::terminate(std::chrono::milliseconds grace) {
  if (m_pid <= 0 || reap()) {
    join_reader();
    return m_state->m_exit_code.load();
  }
  ::kill(m_pid, SIGTERM);
  if (auto code = wait_for_exit(grace); code.has_value()) {
    return code;
  }
  kill();
  reap_blocking();
  join_reader();
  return std::nullopt;
}

void process_runner::kill() {
  if (m_pid > 0 && !m_state->m_reaped.load()) {
    ::kill(m_pid, SIGKILL);
  }
}

} // namespace qoderz::test
