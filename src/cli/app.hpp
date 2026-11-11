#pragma once

#include "common/error.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <expected>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::cli {

/// Everything `main` observes about the process, so a test can drive the whole CLI without a real
/// terminal, real environment or real standard streams.
struct app_context {
  std::vector<std::string> m_args;
  std::map<std::string, std::string> m_env;
  std::ostream& m_out;
  std::ostream& m_err;
  std::istream& m_in;
  bool m_stdin_tty = false;
  bool m_stdout_tty = false;
};

/// The exit codes of `docs/design/cli.md` §11.
enum class exit_code : int {
  success = 0,
  configuration = 1,
  usage = 2,
  auth = 3,
  upstream = 4,
  interrupted = 130,
};

[[nodiscard]] constexpr int code_of(exit_code value) noexcept {
  return static_cast<int>(value);
}

/// Configuration, I/O and internal failures are 1, a bad request is 2, auth is 3, and
/// quota/busy/upstream/protocol are 4; a cancelled command is the Ctrl-C code.
[[nodiscard]] int exit_code_for(error const& value) noexcept;

/// Writes `error: <message>` to stderr and returns the mapped exit code.
int report_error(app_context& context, error const& value);

/// Runs one command line and returns the process exit code. Never throws: an exception escaping a
/// command is an internal failure and becomes exit code 1.
int run(app_context& context);

/// Drives one command coroutine to completion on its own single-threaded io_context. Everything
/// the coroutine references must outlive this call, including `io`.
template <class value_type>
[[nodiscard]] result<value_type> run_blocking(boost::asio::io_context& io,
                                              boost::asio::awaitable<result<value_type>> work) {
  std::optional<result<value_type>> outcome;
  // A command may drive several coroutines over one io_context, and `run` returns once the work
  // queue drains, so the context has to be restarted before the next one.
  io.restart();
  boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
        outcome.emplace(co_await std::move(work));
        co_return;
      },
      boost::asio::detached);
  io.run();
  if (!outcome.has_value()) {
    return std::unexpected(
        make_error(error_kind::internal, "the command coroutine ended without a result"));
  }
  return std::move(*outcome);
}

} // namespace qoderz::cli
