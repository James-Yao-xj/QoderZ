#include "config/paths.hpp"

#include "common/strings.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace qoderz::config {
namespace {

constexpr std::string_view app_directory = "qoderz";
constexpr std::string_view credentials_file_name = "credentials.json";

bool has_value(std::optional<std::string> const& value) {
  return value.has_value() && !strings::trim(*value).empty();
}

std::filesystem::path join(std::filesystem::path base, std::string_view leaf) {
  return base / std::string{app_directory} / std::string{leaf};
}

} // namespace

result<std::filesystem::path>
default_credentials_path(std::optional<std::string> const& xdg_config_home,
                         std::optional<std::string> const& home,
                         std::optional<std::string> const& app_data) {
  if (has_value(xdg_config_home)) {
    std::filesystem::path const base{*xdg_config_home};
    if (base.is_absolute()) {
      return join(base, credentials_file_name);
    }
  }
  if (has_value(home)) {
    return join(std::filesystem::path{*home} / ".config", credentials_file_name);
  }
  if (has_value(app_data)) {
    return join(std::filesystem::path{*app_data}, credentials_file_name);
  }
  return std::unexpected(configuration_error(
      "Cannot locate the credential file; set QODERZ_CREDENTIALS or --credentials, or define "
      "XDG_CONFIG_HOME, HOME or APPDATA."));
}

result<std::filesystem::path> resolve_credentials_path(
    std::optional<std::string> const& explicit_path, std::optional<std::string> const& env_path,
    std::optional<std::string> const& xdg_config_home, std::optional<std::string> const& home,
    std::optional<std::string> const& app_data) {
  if (has_value(explicit_path)) {
    return std::filesystem::path{*explicit_path};
  }
  if (has_value(env_path)) {
    return std::filesystem::path{*env_path};
  }
  return default_credentials_path(xdg_config_home, home, app_data);
}

result<void> ensure_private_directory(std::filesystem::path const& path) {
  if (path.empty()) {
    return std::unexpected(
        configuration_error("Cannot create a private directory: the path is empty."));
  }

  std::error_code error;
  auto const status = std::filesystem::status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(
        configuration_error("Could not inspect '" + path.string() + "': " + error.message()));
  }

  if (!std::filesystem::exists(status)) {
    if (auto const parent = path.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent, error);
      if (error) {
        return std::unexpected(
            configuration_error("Could not create '" + parent.string() + "': " + error.message()));
      }
    }
#if defined(_WIN32)
    std::filesystem::create_directory(path, error);
    if (error) {
      return std::unexpected(
          configuration_error("Could not create '" + path.string() + "': " + error.message()));
    }
#else
    // mkdir rather than create_directories, so the leaf never exists with a looser mode, and
    // chmod because the mode passed to mkdir is still masked by the process umask.
    if (::mkdir(path.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
      return std::unexpected(configuration_error("Could not create '" + path.string() +
                                                 "': " + std::generic_category().message(errno)));
    }
#endif
  } else if (!std::filesystem::is_directory(status)) {
    return std::unexpected(
        configuration_error("'" + path.string() + "' exists but is not a directory."));
  }

#if !defined(_WIN32)
  // chmod because the mode passed to mkdir is masked by the process umask, and because an existing
  // directory QoderZ owns is tightened to 0700. Do not call this for a user-chosen directory.
  if (::chmod(path.c_str(), S_IRWXU) != 0) {
    return std::unexpected(configuration_error("Could not restrict permissions on '" +
                                               path.string() +
                                               "': " + std::generic_category().message(errno)));
  }
#endif
  return {};
}
result<void> require_directory(std::filesystem::path const& path) {
  if (path.empty()) {
    return std::unexpected(
        configuration_error("Cannot prepare the credential directory: the path is empty."));
  }

  std::error_code error;
  auto const status = std::filesystem::status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return std::unexpected(
        configuration_error("Could not inspect '" + path.string() + "': " + error.message()));
  }
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_directory(status)) {
      return std::unexpected(
          configuration_error("'" + path.string() + "' exists but is not a directory."));
    }
    return {};
  }

  // Creating a missing directory is fine; the mode is only 0700 when QoderZ is the one making it.
  return ensure_private_directory(path);
}

} // namespace qoderz::config
