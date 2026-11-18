#include "openai/selection/session.hpp"

#include "common/codec/hash.hpp"
#include "common/codec/random.hpp"
#include "common/strings.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::openai::selection {
namespace {

constexpr std::size_t session_hash_length = 32;
constexpr std::size_t task_name_scalars = 10;

void append_framed(codec::sha256& buffer, std::string_view value) {
  auto const size = static_cast<std::uint32_t>(value.size());
  std::array<char, 4> const length{
      static_cast<char>(size & 0xFFU), static_cast<char>((size >> 8U) & 0xFFU),
      static_cast<char>((size >> 16U) & 0xFFU), static_cast<char>((size >> 24U) & 0xFFU)};
  buffer.update(std::string_view{length.data(), length.size()});
  buffer.update(value);
}

void append_index(codec::sha256& buffer, int value) {
  auto const bits = static_cast<std::uint32_t>(value);
  std::array<char, 4> const bytes{
      static_cast<char>(bits & 0xFFU), static_cast<char>((bits >> 8U) & 0xFFU),
      static_cast<char>((bits >> 16U) & 0xFFU), static_cast<char>((bits >> 24U) & 0xFFU)};
  buffer.update(std::string_view{bytes.data(), bytes.size()});
}

std::string as_uuid_shape(std::string const& hex) {
  std::string uuid;
  uuid.reserve(36);
  for (std::size_t index = 0; index < session_hash_length; ++index) {
    if (index == 8 || index == 12 || index == 16 || index == 20) {
      uuid.push_back('-');
    }
    uuid.push_back(hex[index]);
  }
  return uuid;
}

std::size_t scalar_width(unsigned char lead) noexcept {
  if ((lead & 0xF8U) == 0xF0U) {
    return 4;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2;
  }
  return 1;
}

std::string task_name(std::string_view prompt) {
  std::size_t taken = 0;
  std::size_t index = 0;
  while (index < prompt.size() && taken < task_name_scalars) {
    index +=
        std::min(scalar_width(static_cast<unsigned char>(prompt[index])), prompt.size() - index);
    ++taken;
  }
  return std::string{prompt.substr(0, index)};
}

std::string turn_key(std::string_view session_id, int last_user_index,
                     std::string_view last_user_text) {
  codec::sha256 buffer;
  append_framed(buffer, session_id);
  append_index(buffer, last_user_index);
  append_framed(buffer, last_user_text);
  return buffer.hex_digest();
}

std::string_view trimmed_or_empty(std::optional<std::string_view> value) {
  return value.has_value() ? strings::trim(*value) : std::string_view{};
}

} // namespace

std::string make_uuid() {
  auto const raw = codec::random_bytes(16);
  constexpr std::string_view digits = "0123456789abcdef";

  std::array<char, 32> hex{};
  for (std::size_t index = 0; index < raw.size(); ++index) {
    auto byte = static_cast<unsigned char>(raw[index]);
    if (index == 6) {
      byte = static_cast<unsigned char>((byte & 0x0FU) | 0x40U);
    } else if (index == 8) {
      byte = static_cast<unsigned char>((byte & 0x3FU) | 0x80U);
    }
    hex[index * 2] = digits[byte >> 4U];
    hex[(index * 2) + 1] = digits[byte & 0x0FU];
  }

  std::string uuid;
  uuid.reserve(36);
  for (std::size_t index = 0; index < hex.size(); ++index) {
    if (index == 8 || index == 12 || index == 16 || index == 20) {
      uuid.push_back('-');
    }
    uuid.push_back(hex[index]);
  }
  return uuid;
}

session_store::session_store(std::size_t capacity, std::int64_t ttl_ms)
    : m_capacity{capacity == 0 ? 1 : capacity}, m_ttl_ms{ttl_ms} {}

std::string session_store::make_session_id(std::optional<std::string_view> named,
                                           std::optional<std::string_view> cache_key,
                                           std::optional<std::string_view> user,
                                           std::string_view system_text,
                                           std::string_view first_user_text) const {
  codec::sha256 buffer;

  auto const named_text = trimmed_or_empty(named);
  auto const key_text = trimmed_or_empty(cache_key);
  auto const user_text = trimmed_or_empty(user);

  // The tag is framed like the values, so that a key of "chat" in the `user` slot cannot be
  // confused with the fallback branch's own tag.
  if (!named_text.empty()) {
    append_framed(buffer, "session");
    append_framed(buffer, named_text);
  } else if (!key_text.empty()) {
    append_framed(buffer, "key");
    append_framed(buffer, key_text);
  } else if (!user_text.empty()) {
    append_framed(buffer, "user");
    append_framed(buffer, user_text);
  } else {
    append_framed(buffer, "chat");
    append_framed(buffer, system_text);
    append_framed(buffer, first_user_text);
  }

  return as_uuid_shape(buffer.hex_digest());
}

chat_task session_store::get_or_create(std::string_view session_id, int last_user_index,
                                       std::string_view last_user_text, std::int64_t now_ms) {
  auto const key = turn_key(session_id, last_user_index, last_user_text);

  std::lock_guard<std::mutex> guard{m_gate};
  evict_expired(now_ms);

  if (auto const found = m_tasks.find(key); found != m_tasks.end()) {
    found->second.m_last_used_ms = now_ms;
    return found->second.m_task;
  }

  while (m_tasks.size() >= m_capacity) {
    evict_least_recent();
  }

  entry fresh{};
  fresh.m_task.m_id = make_uuid();
  fresh.m_task.m_begin_at_ms = now_ms;
  fresh.m_task.m_answered = false;
  fresh.m_task.m_name = task_name(last_user_text);
  fresh.m_last_used_ms = now_ms;

  auto task = fresh.m_task;
  m_tasks.emplace(key, std::move(fresh));
  return task;
}

void session_store::mark_answered(std::string_view task_id) {
  if (task_id.empty()) {
    return;
  }

  std::lock_guard<std::mutex> guard{m_gate};
  for (auto& slot : m_tasks) {
    if (slot.second.m_task.m_id == task_id) {
      slot.second.m_task.m_answered = true;
      return;
    }
  }
}

std::size_t session_store::size() const {
  std::lock_guard<std::mutex> guard{m_gate};
  return m_tasks.size();
}

void session_store::evict_expired(std::int64_t now_ms) {
  for (auto slot = m_tasks.begin(); slot != m_tasks.end();) {
    if (now_ms - slot->second.m_last_used_ms > m_ttl_ms) {
      slot = m_tasks.erase(slot);
    } else {
      ++slot;
    }
  }
}

void session_store::evict_least_recent() {
  auto oldest = m_tasks.end();
  for (auto slot = m_tasks.begin(); slot != m_tasks.end(); ++slot) {
    if (oldest == m_tasks.end() || slot->second.m_last_used_ms < oldest->second.m_last_used_ms) {
      oldest = slot;
    }
  }
  if (oldest != m_tasks.end()) {
    m_tasks.erase(oldest);
  }
}

} // namespace qoderz::openai::selection
