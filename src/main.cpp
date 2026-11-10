#include "common/version.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view program_name = "qoderz";
constexpr std::string_view program_description = "Qoder to OpenAI-compatible gateway";

} // namespace

int main(int argc, char** argv) {
  CLI::App app{std::string{program_description}, std::string{program_name}};
  app.set_version_flag("-v,--version",
                       std::string{program_name} + " " + std::string{qoderz::version::string});

  CLI11_PARSE(app, argc, argv);

  if (argc == 1) {
    std::cout << app.help();
  }
  return 0;
}
