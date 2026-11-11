#include "qoder/auth/support/browser.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace qoderz::qoder::auth {
namespace {

std::unexpected<error> launch_error(std::string_view detail) {
  return std::unexpected(
      make_error(error_kind::io, "The browser could not be started: " + std::string{detail}));
}

} // namespace

std::vector<std::string> browser_command(std::string_view url) {
  std::string target{url};
#if defined(_WIN32)
  return {"cmd", "/c", "start", "", std::move(target)};
#elif defined(__APPLE__)
  return {"open", std::move(target)};
#else
  return {"xdg-open", std::move(target)};
#endif
}

#if defined(_WIN32)

result<void> open_browser(std::string_view url) {
  auto const command = browser_command(url);
  std::vector<char const*> argv;
  argv.reserve(command.size() + 1);
  for (auto const& argument : command) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);
  if (::_spawnvp(_P_NOWAIT, argv[0], argv.data()) < 0) {
    return launch_error("the launcher could not be spawned");
  }
  return {};
}

#else

namespace {

/// A child that fails to exec must not look like a launcher that ran and exited cleanly.
constexpr int exec_failed_exit_code = 127;

} // namespace

result<void> open_browser(std::string_view url) {
  auto const command = browser_command(url);
  if (command.empty()) {
    return launch_error("there is no launcher for this platform");
  }

  std::vector<char*> argv;
  argv.reserve(command.size() + 1);
  for (auto const& argument : command) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  auto const pid = ::fork();
  if (pid < 0) {
    return launch_error("fork failed");
  }
  if (pid == 0) {
    /* The child would otherwise inherit the parent's descriptors, including the credential file
       this process may hold open, and write to the CLI's terminal after it has moved on. */
    auto const null_descriptor = ::open("/dev/null", O_RDWR);
    if (null_descriptor >= 0) {
      ::dup2(null_descriptor, STDIN_FILENO);
      ::dup2(null_descriptor, STDOUT_FILENO);
      ::dup2(null_descriptor, STDERR_FILENO);
      if (null_descriptor > STDERR_FILENO) {
        ::close(null_descriptor);
      }
    }
    ::execvp(argv[0], argv.data());
    ::_exit(exec_failed_exit_code);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return {};
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return launch_error("the launcher reported a failure");
  }
  return {};
}

#endif

} // namespace qoderz::qoder::auth
