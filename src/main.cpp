#include "cli/app.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

extern char** environ;

namespace {

bool is_tty(int descriptor) {
#if defined(_WIN32)
  return _isatty(descriptor) != 0;
#else
  return isatty(descriptor) != 0;
#endif
}

std::map<std::string, std::string> read_environment() {
  std::map<std::string, std::string> env;
  if (environ == nullptr) {
    return env;
  }
  for (char** entry = environ; *entry != nullptr; ++entry) {
    std::string pair{*entry};
    auto const separator = pair.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    env.emplace(pair.substr(0, separator), pair.substr(separator + 1));
  }
  return env;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  qoderz::cli::app_context context{.m_args = std::move(args),
                                   .m_env = read_environment(),
                                   .m_out = std::cout,
                                   .m_err = std::cerr,
                                   .m_in = std::cin,
                                   .m_stdin_tty = is_tty(0),
                                   .m_stdout_tty = is_tty(1)};
  try {
    return qoderz::cli::run(context);
  } catch (std::exception const& escaped) {
    context.m_err << "error: " << escaped.what() << '\n';
    return qoderz::cli::code_of(qoderz::cli::exit_code::configuration);
  } catch (...) {
    context.m_err << "error: an unexpected internal failure occurred\n";
    return qoderz::cli::code_of(qoderz::cli::exit_code::configuration);
  }
}
