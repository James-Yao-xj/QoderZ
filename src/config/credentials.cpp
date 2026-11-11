#include "config/credentials.hpp"

#include "common/codec/random.hpp"
#include "common/json.hpp"
#include "common/strings.hpp"
#include "config/accounts.hpp"
#include "config/paths.hpp"
#include "config/site.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qoderz::config {
namespace {

constexpr int credential_version = 1;
constexpr std::uintmax_t max_file_bytes = 16 * 1024 * 1024;

std::unexpected<error> file_error(std::filesystem::path const& path, std::string_view detail) {
  return std::unexpected(configuration_error(
      "The credential file at '" + path.string() + "' is not in the supported format (" +
      std::string{detail} +
      "). Delete it, or point --credentials at a new path, then run: qoderz login"));
}

std::unexpected<error> io_error(std::string_view action, std::filesystem::path const& path,
                                std::string const& detail) {
  return std::unexpected(configuration_error("Could not " + std::string{action} + " '" +
                                             path.string() + "': " + detail));
}

result<std::string> read_text(std::filesystem::path const& path) {
  std::error_code error;
  auto const size = std::filesystem::file_size(path, error);
  if (error) {
    return io_error("read", path, error.message());
  }
  if (size > max_file_bytes) {
    return std::unexpected(
        configuration_error("The credential file at '" + path.string() +
                            "' is larger than 16 MiB; a credential file holds a handful of "
                            "accounts, so this is not a file QoderZ wrote."));
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return io_error("open", path, std::generic_category().message(errno));
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  stream.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream) {
    return io_error("read", path, "the read stopped before the end of the file");
  }
  return text;
}

struct json_field_reader {
  json const& m_entry;
  std::string const& m_key;
  std::string m_problem;

  [[nodiscard]] json const* lookup(std::string_view field) const {
    auto const it = m_entry.find(std::string{field});
    return it == m_entry.end() ? nullptr : &*it;
  }

  bool fail(std::string_view field, std::string_view expected) {
    m_problem = "field '" + std::string{field} + "' of account '" + m_key + "' is not " +
                std::string{expected};
    return false;
  }

  bool read_string(std::string_view field, std::string& out) {
    auto const* value = lookup(field);
    if (value == nullptr) {
      return true;
    }
    if (!value->is_string()) {
      return fail(field, "a string");
    }
    out = value->get<std::string>();
    return true;
  }

  bool read_int64(std::string_view field, std::int64_t& out) {
    auto const* value = lookup(field);
    if (value == nullptr) {
      return true;
    }
    if (!value->is_number_integer()) {
      return fail(field, "an integer");
    }
    if (value->is_number_unsigned()) {
      auto const number = value->get<std::uint64_t>();
      if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(field, "an integer that fits in 64 signed bits");
      }
      out = static_cast<std::int64_t>(number);
      return true;
    }
    out = value->get<std::int64_t>();
    return true;
  }

  bool read_bool(std::string_view field, bool& out) {
    auto const* value = lookup(field);
    if (value == nullptr) {
      return true;
    }
    if (!value->is_boolean()) {
      return fail(field, "a boolean");
    }
    out = value->get<bool>();
    return true;
  }
};

result<account_record> parse_account(std::string const& key, json const& entry,
                                     std::filesystem::path const& path) {
  if (!entry.is_object()) {
    return file_error(path, "account '" + key + "' is not an object");
  }
  account_record account;
  json_field_reader reader{.m_entry = entry, .m_key = key, .m_problem = {}};
  bool const parsed = reader.read_string("site", account.m_site) &&
                      reader.read_string("name", account.m_name) &&
                      reader.read_string("vpc", account.m_vpc) &&
                      reader.read_string("access_token", account.m_access_token) &&
                      reader.read_string("refresh_token", account.m_refresh_token) &&
                      reader.read_int64("expires_at", account.m_expires_at) &&
                      reader.read_string("uid", account.m_uid) &&
                      reader.read_string("display_name", account.m_display_name) &&
                      reader.read_string("email", account.m_email) &&
                      reader.read_string("account_id", account.m_account_id) &&
                      reader.read_string("machine_id", account.m_machine_id) &&
                      reader.read_string("device_token", account.m_device_token) &&
                      reader.read_string("device_refresh_token", account.m_device_refresh_token) &&
                      reader.read_bool("device_chat", account.m_device_chat) &&
                      reader.read_bool("pat_auth", account.m_pat_auth) &&
                      reader.read_string("pat", account.m_pat) &&
                      reader.read_int64("created_at", account.m_created_at);
  if (!parsed) {
    return file_error(path, reader.m_problem);
  }
  return account;
}

result<credential_file> parse_document(json const& document, std::filesystem::path const& path) {
  if (!document.is_object()) {
    return file_error(path, "the document is not a JSON object");
  }
  auto const version = document.find("version");
  if (version == document.end()) {
    return std::unexpected(
        configuration_error("The credential file at '" + path.string() +
                            "' declares no version; version 1 is required. Delete it, or "
                            "point --credentials at a new path, then run: qoderz login"));
  }
  if (!version->is_number_integer()) {
    return file_error(path, "its 'version' member is not an integer");
  }
  bool const supported =
      version->is_number_unsigned()
          ? version->get<std::uint64_t>() == static_cast<std::uint64_t>(credential_version)
          : version->get<std::int64_t>() == credential_version;
  if (!supported) {
    return std::unexpected(configuration_error(
        "The credential file at '" + path.string() + "' declares version " + version->dump() +
        "; version 1 is required. Delete it, or point --credentials at a new path, then run: "
        "qoderz login"));
  }

  credential_file file;
  file.m_version = credential_version;

  auto const accounts = document.find("accounts");
  if (accounts != document.end()) {
    if (!accounts->is_object()) {
      return file_error(path, "its 'accounts' member is not an object");
    }
    for (auto const& [key, entry] : accounts->items()) {
      auto parsed = parse_account(key, entry, path);
      if (!parsed) {
        return std::unexpected(parsed.error());
      }
      file.m_accounts.emplace(key, std::move(*parsed));
    }
  }

  auto const defaults = document.find("defaults");
  if (defaults != document.end()) {
    if (!defaults->is_object()) {
      return file_error(path, "its 'defaults' member is not an object");
    }
    for (auto const& [site, key] : defaults->items()) {
      if (!key.is_string()) {
        return file_error(path, "the default for site '" + site + "' is not a string");
      }
      file.m_defaults.emplace(site, key.get<std::string>());
    }
  }
  return file;
}

result<void> validate_file(credential_file const& file, std::filesystem::path const& path) {
  if (file.m_version != credential_version) {
    return std::unexpected(
        configuration_error("The credential file at '" + path.string() + "' declares version " +
                            std::to_string(file.m_version) + "; version 1 is required."));
  }
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> names_by_site;
  for (auto const& [key, account] : file.m_accounts) {
    if (account.m_site.empty() || account.m_name.empty()) {
      return file_error(path, "account '" + key + "' has no site or no name");
    }
    if (!is_valid_site_id(account.m_site)) {
      return file_error(path, "account '" + key + "' has site '" + account.m_site +
                                  "', which is not a site id (lowercase letters, digits and '-')");
    }
    if (!is_valid_account_name(account.m_name)) {
      return file_error(path, "account '" + key + "' has an invalid name");
    }
    if (key != account.key()) {
      return file_error(path, "account '" + key + "' does not match site '" + account.m_site +
                                  "' and name '" + account.m_name + "'");
    }
    auto& names = names_by_site[account.m_site];
    if (!names.insert(strings::ascii_lower_copy(account.m_name)).second) {
      return file_error(path, "site '" + account.m_site + "' has more than one account named '" +
                                  account.m_name + "'");
    }
  }
  for (auto const& [site, key] : file.m_defaults) {
    auto const account = file.m_accounts.find(key);
    if (account == file.m_accounts.end()) {
      return file_error(path, "the default for site '" + site + "' points at '" + key +
                                  "', which does not exist");
    }
    if (account->second.m_site != site) {
      return file_error(path, "the default for site '" + site + "' points at an account of site '" +
                                  account->second.m_site + "'");
    }
  }
  return {};
}

json to_document(credential_file const& file) {
  auto accounts = json::object();
  for (auto const& [key, account] : file.m_accounts) {
    accounts[key] = json{
        {"site", account.m_site},
        {"name", account.m_name},
        {"vpc", account.m_vpc},
        {"access_token", account.m_access_token},
        {"refresh_token", account.m_refresh_token},
        {"expires_at", account.m_expires_at},
        {"uid", account.m_uid},
        {"display_name", account.m_display_name},
        {"email", account.m_email},
        {"account_id", account.m_account_id},
        {"machine_id", account.m_machine_id},
        {"device_token", account.m_device_token},
        {"device_refresh_token", account.m_device_refresh_token},
        {"device_chat", account.m_device_chat},
        {"pat_auth", account.m_pat_auth},
        {"pat", account.m_pat},
        {"created_at", account.m_created_at},
    };
  }
  auto defaults = json::object();
  for (auto const& [site, key] : file.m_defaults) {
    defaults[site] = key;
  }
  json document;
  document["version"] = file.m_version;
  document["accounts"] = std::move(accounts);
  document["defaults"] = std::move(defaults);
  return document;
}

struct temporary_file {
  std::filesystem::path m_path;

  temporary_file() = default;
  temporary_file(temporary_file const&) = delete;
  temporary_file& operator=(temporary_file const&) = delete;
  ~temporary_file() {
    if (!m_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(m_path, ignored);
    }
  }
};

#if !defined(_WIN32)

struct descriptor_handle {
  int m_fd = -1;

  descriptor_handle() = default;
  descriptor_handle(descriptor_handle const&) = delete;
  descriptor_handle& operator=(descriptor_handle const&) = delete;
  ~descriptor_handle() {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
  }
};

result<void> sync_directory(std::filesystem::path const& directory) {
  descriptor_handle handle;
  handle.m_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (handle.m_fd < 0) {
    return io_error("open", directory,
                    std::generic_category().message(errno) + " (to flush the directory)");
  }
  if (::fsync(handle.m_fd) != 0) {
    return io_error("flush", directory, std::generic_category().message(errno));
  }
  return {};
}

result<void> write_atomically(std::filesystem::path const& path, std::string const& text) {
  auto const directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (auto const prepared = require_directory(directory); !prepared) {
    return std::unexpected(prepared.error());
  }

  temporary_file temporary;
  temporary.m_path =
      directory / ("." + path.filename().string() + "." + codec::random_hex(8) + ".tmp");
  descriptor_handle handle;
  handle.m_fd = ::open(temporary.m_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (handle.m_fd < 0) {
    return io_error("create", temporary.m_path, std::generic_category().message(errno));
  }

  auto remaining = std::string_view{text};
  while (!remaining.empty()) {
    auto const written = ::write(handle.m_fd, remaining.data(), remaining.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return io_error("write", temporary.m_path, std::generic_category().message(errno));
    }
    remaining.remove_prefix(static_cast<std::size_t>(written));
  }
  if (::fsync(handle.m_fd) != 0) {
    return io_error("flush", temporary.m_path, std::generic_category().message(errno));
  }
  auto const descriptor = handle.m_fd;
  handle.m_fd = -1;
  if (::close(descriptor) != 0) {
    return io_error("close", temporary.m_path, std::generic_category().message(errno));
  }

  std::error_code error;
  std::filesystem::rename(temporary.m_path, path, error);
  if (error) {
    return io_error("replace", path, error.message());
  }
  temporary.m_path.clear();
  return sync_directory(directory);
}

#else

result<void> write_atomically(std::filesystem::path const& path, std::string const& text) {
  auto const directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
  if (auto const prepared = require_directory(directory); !prepared) {
    return std::unexpected(prepared.error());
  }

  temporary_file temporary;
  temporary.m_path =
      directory / ("." + path.filename().string() + "." + codec::random_hex(8) + ".tmp");
  {
    std::ofstream stream{temporary.m_path, std::ios::binary | std::ios::trunc};
    if (!stream) {
      return io_error("create", temporary.m_path, std::generic_category().message(errno));
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) {
      return io_error("write", temporary.m_path, "the write did not complete");
    }
  }

  std::error_code error;
  std::filesystem::remove(path, error);
  error.clear();
  std::filesystem::rename(temporary.m_path, path, error);
  if (error) {
    return io_error("replace", path, error.message());
  }
  temporary.m_path.clear();
  return {};
}

#endif

} // namespace

std::string account_record::key() const {
  return m_site + "/" + m_name;
}

credential_store::credential_store(std::filesystem::path path) : m_path{std::move(path)} {}

std::filesystem::path const& credential_store::path() const noexcept {
  return m_path;
}

result<credential_file> credential_store::load() const {
  std::error_code error;
  auto const status = std::filesystem::status(m_path, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return credential_file{};
    }
    return io_error("inspect", m_path, error.message());
  }
  if (!std::filesystem::exists(status)) {
    return credential_file{};
  }
  if (!std::filesystem::is_regular_file(status)) {
    return std::unexpected(configuration_error("The credential file at '" + m_path.string() +
                                               "' is not a regular file."));
  }

  auto const text = read_text(m_path);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto const document = json::parse(*text, nullptr, false);
  if (document.is_discarded()) {
    return file_error(m_path, "it is not valid JSON");
  }
  auto parsed = parse_document(document, m_path);
  if (!parsed) {
    return parsed;
  }
  if (auto const valid = validate_file(*parsed, m_path); !valid) {
    return std::unexpected(valid.error());
  }
  return parsed;
}

result<void> credential_store::save(credential_file const& file) const {
  if (auto const valid = validate_file(file, m_path); !valid) {
    return std::unexpected(valid.error());
  }
  std::error_code error;
  bool const exists = std::filesystem::exists(m_path, error);
  if (error) {
    return io_error("inspect", m_path, error.message());
  }
  if (exists) {
    if (auto const existing = load(); !existing) {
      return std::unexpected(existing.error());
    }
  }
  auto const text = to_document(file).dump(2) + "\n";
  return write_atomically(m_path, text);
}

result<void> credential_store::remove_file() const {
  std::error_code error;
  bool const removed = std::filesystem::remove(m_path, error);
  if (error) {
    return io_error("remove", m_path, error.message());
  }
  if (!removed) {
    return {};
  }
#if !defined(_WIN32)
  // Windows has no directory handle to flush, so the removed file is already durable there.
  auto const directory =
      m_path.has_parent_path() ? m_path.parent_path() : std::filesystem::path{"."};
  return sync_directory(directory);
#else
  return {};
#endif
}

} // namespace qoderz::config
