#include "common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace qoderz {
namespace {

std::atomic<log_level> active_level{log_level::info};
std::mutex output_mutex;

std::string timestamp_utc() {
  auto const now = std::chrono::system_clock::now();
  auto const millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t const seconds = std::chrono::system_clock::to_time_t(now);

  std::tm utc{};
  gmtime_r(&seconds, &utc);

  char date_part[32];
  std::strftime(date_part, sizeof(date_part), "%Y-%m-%dT%H:%M:%S", &utc);

  char full[48];
  std::snprintf(full, sizeof(full), "%s.%03dZ", date_part, static_cast<int>(millis.count()));
  return full;
}

bool needs_quotes(std::string_view value) {
  if (value.empty()) {
    return true;
  }
  return value.find_first_of(" \t\r\n=\"") != std::string_view::npos;
}

void append_value(std::string& line, std::string_view value) {
  if (!needs_quotes(value)) {
    line.append(value);
    return;
  }
  line.push_back('"');
  for (char const character : value) {
    if (character == '"' || character == '\\') {
      line.push_back('\\');
    }
    line.push_back(character);
  }
  line.push_back('"');
}

} // namespace

void set_log_level(log_level level) noexcept {
  active_level.store(level, std::memory_order_relaxed);
}

log_level current_log_level() noexcept {
  return active_level.load(std::memory_order_relaxed);
}

void log(log_level level, std::string_view message, log_fields fields) {
  if (static_cast<int>(level) < static_cast<int>(active_level.load(std::memory_order_relaxed))) {
    return;
  }

  std::string line;
  line.reserve(64 + message.size() + fields.size() * 24);
  line += timestamp_utc();
  line += " [";
  line.append(to_string(level));
  line += "] ";
  line.append(message);

  for (log_field const& field : fields) {
    line.push_back(' ');
    line.append(field.m_key);
    line.push_back('=');
    append_value(line, field.m_value);
  }
  line.push_back('\n');

  std::lock_guard const lock(output_mutex);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

} // namespace qoderz
