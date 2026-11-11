#pragma once

#include "openai/request/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qoderz::openai::selection {

/// A random, hyphenated 8-4-4-4-12 identifier. Random rather than derived: a derived one would leak
/// the inputs it was folded from, and nothing about a request id has to be reproducible.
[[nodiscard]] std::string make_uuid();

/// Session ids and the task table of one process, per `docs/protocol/openai-compat.md` §4.
///
/// Session ids are process-local state, not a persisted contract: the value may change between
/// builds without being a compatibility bug. Only stability inside one process matters.
///
/// Every member is safe to call from several threads; `get_or_create` returns a task by value.
class session_store {
public:
  explicit session_store(std::size_t capacity = 512, std::int64_t ttl_ms = 2 * 60 * 60 * 1000);

  /// header → `prompt_cache_key` → `user` → `system_text + first_user_text`, per §4.1: the first
  /// three are trimmed, falling through to the next on a blank value; the two prompt texts are not,
  /// because their whitespace is part of the prompt.
  [[nodiscard]] std::string make_session_id(std::optional<std::string_view> named,
                                            std::optional<std::string_view> cache_key,
                                            std::optional<std::string_view> user,
                                            std::string_view system_text,
                                            std::string_view first_user_text) const;

  /// The task of this turn. A hit refreshes its age, so a client that keeps asking about the same
  /// turn cannot lose the task to the sweeper.
  [[nodiscard]] chat_task get_or_create(std::string_view session_id, int last_user_index,
                                        std::string_view last_user_text, std::int64_t now_ms);

  /// An empty or already-expired id is ignored: a finished response and a task that expired are
  /// both ordinary, and neither should turn into an error the client sees.
  void mark_answered(std::string_view task_id);

  [[nodiscard]] std::size_t size() const;

private:
  struct entry {
    chat_task m_task;
    std::int64_t m_last_used_ms = 0;
  };

  void evict_expired(std::int64_t now_ms);
  void evict_least_recent();

  mutable std::mutex m_gate;
  std::size_t m_capacity;
  std::int64_t m_ttl_ms;
  std::unordered_map<std::string, entry> m_tasks;
};

} // namespace qoderz::openai::selection
