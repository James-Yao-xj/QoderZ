#pragma once

#include <filesystem>
#include <string_view>

namespace qoderz::testing {

/// RAII temporary directory under the system temp directory. Tests write only inside it, so the
/// real home directory and the real credential file are never touched; a cleanup failure is
/// ignored because a destructor must not fail a test.
class temp_dir {
public:
  temp_dir();
  ~temp_dir();

  temp_dir(temp_dir const&) = delete;
  temp_dir& operator=(temp_dir const&) = delete;

  [[nodiscard]] std::filesystem::path const& path() const noexcept;

  /// A child path; nothing is created.
  [[nodiscard]] std::filesystem::path file(std::string_view name) const;

private:
  std::filesystem::path m_path;
};

} // namespace qoderz::testing
