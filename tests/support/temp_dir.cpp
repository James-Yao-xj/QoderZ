#include "support/temp_dir.hpp"

#include "common/codec/random.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace qoderz::testing {

temp_dir::temp_dir() {
  auto const base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 16; ++attempt) {
    auto const candidate = base / ("qoderz-test-" + codec::random_hex(12));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      m_path = candidate;
      return;
    }
  }
  throw std::runtime_error("could not create a temporary directory for the tests");
}

temp_dir::~temp_dir() {
  if (m_path.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove_all(m_path, ignored);
}

std::filesystem::path const& temp_dir::path() const noexcept {
  return m_path;
}

std::filesystem::path temp_dir::file(std::string_view name) const {
  return m_path / std::string{name};
}

} // namespace qoderz::testing
