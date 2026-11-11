#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace qoderz::config {

/// One stored account. Every field maps to the snake_case JSON field of the version 1 file
/// described in docs/design/accounts.md §2.2; a field missing from the JSON loads as an empty
/// string, 0 or false. Access/refresh tokens and the PAT are secret and never logged or echoed.
struct account_record {
  std::string m_site;
  std::string m_name;
  std::string m_vpc;
  std::string m_access_token;
  std::string m_refresh_token;
  std::int64_t m_expires_at = 0;
  std::string m_uid;
  std::string m_display_name;
  std::string m_email;
  std::string m_account_id;
  std::string m_machine_id;
  std::string m_device_token;
  std::string m_device_refresh_token;
  bool m_device_chat = false;
  bool m_pat_auth = false;
  std::string m_pat;
  std::int64_t m_created_at = 0;

  [[nodiscard]] std::string key() const;
};

struct credential_file {
  int m_version = 1;
  std::map<std::string, account_record, std::less<>> m_accounts;
  std::map<std::string, std::string, std::less<>> m_defaults;
};

/// Reads and writes the credential file; it holds no cache, so every `load` sees the current disk
/// state. `save` validates, writes a 0600 temporary file next to the target, fsyncs it, renames it
/// over the target and fsyncs the directory; a target that cannot be read back is never replaced.
class credential_store {
public:
  explicit credential_store(std::filesystem::path path);

  [[nodiscard]] std::filesystem::path const& path() const noexcept;

  /// Missing file means "no accounts" and returns an empty `credential_file`. A file that is
  /// unreadable, larger than 16 MiB, malformed or internally inconsistent is a configuration
  /// error naming the path and the problem; nothing is repaired and nothing is written.
  [[nodiscard]] result<credential_file> load() const;

  [[nodiscard]] result<void> save(credential_file const& file) const;

  /// Removes the credential file and flushes its directory. A missing file is success.
  [[nodiscard]] result<void> remove_file() const;

private:
  std::filesystem::path m_path;
};

} // namespace qoderz::config
