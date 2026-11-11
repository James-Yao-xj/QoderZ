#pragma once

#include "common/error.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace qoderz::config {

/// `$XDG_CONFIG_HOME/qoderz/credentials.json` when `xdg_config_home` is an absolute path, then
/// `$HOME/.config/qoderz/credentials.json`, then `%APPDATA%\qoderz\credentials.json`. The values
/// are passed in rather than read from the process environment, so tests never touch the real
/// home directory. No location at all is a configuration error.
[[nodiscard]] result<std::filesystem::path>
default_credentials_path(std::optional<std::string> const& xdg_config_home,
                         std::optional<std::string> const& home,
                         std::optional<std::string> const& app_data);

/// Explicit path first, then the `QODERZ_CREDENTIALS` value, then `default_credentials_path`.
/// Blank values count as unset and explicit paths are used verbatim. The configuration error from
/// `default_credentials_path` is propagated, because there is no built-in fallback location.
[[nodiscard]] result<std::filesystem::path> resolve_credentials_path(
    std::optional<std::string> const& explicit_path, std::optional<std::string> const& env_path,
    std::optional<std::string> const& xdg_config_home, std::optional<std::string> const& home,
    std::optional<std::string> const& app_data);

/// Creates `path` with mode 0700 when it is missing, and tightens an existing directory to 0700.
/// An existing non-directory, or any failure, is a configuration error. Call this only for a
/// directory QoderZ owns; a directory the user chose (`--credentials ./credentials.json`) is
/// checked with `require_directory`, because chmod on someone else's directory is a lasting side
/// effect.
[[nodiscard]] result<void> ensure_private_directory(std::filesystem::path const& path);
/// Verifies that `path` exists and is a directory, creating it 0700 when it is missing, but never
/// changing the mode of a directory that already exists. Used for the parent of a credential file,
/// which may be any directory the user picked. An existing non-directory is a configuration error.
[[nodiscard]] result<void> require_directory(std::filesystem::path const& path);

} // namespace qoderz::config
