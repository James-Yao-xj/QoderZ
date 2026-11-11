#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace qoderz::qoder::auth {

struct login_options {
  std::string m_requested_name;
  bool m_open_browser = true;
  std::chrono::milliseconds m_poll_interval{2000};
  std::chrono::milliseconds m_timeout{15 * 60 * 1000};
};

struct progress_event {
  std::string m_url;
  std::string m_instructions;
  std::chrono::steady_clock::duration m_elapsed;
};

using progress_callback = std::function<void(progress_event const&)>;

/// `email` when it is known, otherwise `uid`, with the VPC instance appended as ` (vpc)`.
///
/// The value is for display and for matching an existing account; it is never a storage key, so a
/// missing part only makes it less precise, never invalid.
[[nodiscard]] inline std::string build_account_id(std::string_view email, std::string_view uid,
                                                  std::string_view vpc) {
  std::string identity{email.empty() ? uid : email};
  if (!vpc.empty()) {
    identity += " (";
    identity += vpc;
    identity += ')';
  }
  return identity;
}

} // namespace qoderz::qoder::auth
