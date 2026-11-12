#include "openai/request/types.hpp"
#include "openai/selection/session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

using qoderz::openai::chat_task;
using qoderz::openai::selection::make_uuid;
using qoderz::openai::selection::session_store;

constexpr std::int64_t start_ms = 1'760'000'000'000;
constexpr std::int64_t hour_ms = 60 * 60 * 1000;
constexpr std::int64_t two_hours_ms = 2 * 60 * 60 * 1000;

bool looks_like_a_uuid(std::string const& text) {
  if (text.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < text.size(); ++index) {
    bool const dash_slot = index == 8 || index == 13 || index == 18 || index == 23;
    if (dash_slot) {
      if (text[index] != '-') {
        return false;
      }
      continue;
    }
    bool const hex =
        (text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("A generated id is a lowercase hyphenated uuid", "[openai][selection][session]") {
  auto const first = make_uuid();
  auto const second = make_uuid();

  CHECK(looks_like_a_uuid(first));
  CHECK(looks_like_a_uuid(second));
  CHECK(first != second);
}

TEST_CASE("The session id has the uuid shape", "[openai][selection][session]") {
  session_store store;

  auto const id = store.make_session_id("s-1", std::nullopt, std::nullopt, "sys", "hi");

  CHECK(looks_like_a_uuid(id));
}

TEST_CASE("The session id hashes the framed tag and value", "[openai][selection][session]") {
  session_store store;

  // Length prefixes are int32 little-endian UTF-8 byte counts, so these are the exact digests of
  // int32(7)+"session" and int32(3)+"abc".
  CHECK(store.make_session_id("abc", std::nullopt, std::nullopt, "", "") ==
        "96093961-5af1-5b71-24d6-d4b074cf9a7b");
  CHECK(store.make_session_id(std::nullopt, "k1", std::nullopt, "", "") ==
        "bcbe7b12-b2bd-0867-cfdd-aa62dbb46374");
  CHECK(store.make_session_id(std::nullopt, std::nullopt, "u1", "", "") ==
        "3ddebfa3-ca19-0552-626f-6a4c55b5892c");
  CHECK(store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "SYS", "FIRST") ==
        "38916df5-e974-0a08-d852-d1bba8bfc3b4");
}

TEST_CASE("The session id prefers the header over every other key",
          "[openai][selection][session]") {
  session_store store;

  auto const header_only = store.make_session_id("h", std::nullopt, std::nullopt, "sys", "first");
  auto const header_wins = store.make_session_id("h", "key", "user", "other", "prompt");

  CHECK(header_only == header_wins);
  CHECK(header_only != store.make_session_id(std::nullopt, "key", "user", "other", "prompt"));
}

TEST_CASE("A blank key falls through to the next one", "[openai][selection][session]") {
  session_store store;

  CHECK(store.make_session_id("  ", "key", "user", "sys", "first") ==
        store.make_session_id(std::nullopt, "key", "user", "sys", "first"));
  CHECK(store.make_session_id(std::nullopt, "", "user", "sys", "first") ==
        store.make_session_id(std::nullopt, std::nullopt, "user", "sys", "first"));
  CHECK(store.make_session_id(std::nullopt, std::nullopt, " ", "sys", "first") ==
        store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "sys", "first"));
}

TEST_CASE("Keys are trimmed but the prompt fallback is not", "[openai][selection][session]") {
  session_store store;

  CHECK(store.make_session_id(" abc ", std::nullopt, std::nullopt, "", "") ==
        store.make_session_id("abc", std::nullopt, std::nullopt, "", ""));
  CHECK(store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "sys", "hi") !=
        store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "sys", "hi "));
}

TEST_CASE("Different inputs produce different session ids", "[openai][selection][session]") {
  session_store store;

  auto const base = store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "sys", "a");
  CHECK(base != store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "sys", "b"));
  CHECK(base != store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "other", "a"));
  // Splitting the same bytes differently between the two prompt fields must not collide.
  CHECK(store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "ab", "c") !=
        store.make_session_id(std::nullopt, std::nullopt, std::nullopt, "a", "bc"));
}

TEST_CASE("The same input produces the same session id", "[openai][selection][session]") {
  session_store first;
  session_store second;

  auto const id = first.make_session_id("s", "k", "u", "sys", "first");

  CHECK(first.make_session_id("s", "k", "u", "sys", "first") == id);
  CHECK(second.make_session_id("s", "k", "u", "sys", "first") == id);
}

TEST_CASE("The same turn reuses its task", "[openai][selection][session]") {
  session_store store;

  auto const first = store.get_or_create("s", 1, "hello", start_ms);
  auto const second = store.get_or_create("s", 1, "hello", start_ms + 1000);

  CHECK(first.m_id == second.m_id);
  CHECK(first.m_begin_at_ms == start_ms);
  CHECK(second.m_begin_at_ms == start_ms);
  CHECK_FALSE(first.m_answered);
  CHECK(store.size() == 1);
}

TEST_CASE("A new prompt gets a new task", "[openai][selection][session]") {
  session_store store;

  auto const first = store.get_or_create("s", 1, "hello", start_ms);
  auto const other_text = store.get_or_create("s", 1, "goodbye", start_ms);
  auto const other_index = store.get_or_create("s", 2, "hello", start_ms);
  auto const other_session = store.get_or_create("t", 1, "hello", start_ms);

  CHECK(first.m_id != other_text.m_id);
  CHECK(first.m_id != other_index.m_id);
  CHECK(first.m_id != other_session.m_id);
  CHECK(store.size() == 4);
}

TEST_CASE("A created task records the clock and the prompt head", "[openai][selection][session]") {
  session_store store;

  auto const task = store.get_or_create("s", 3, "abcdefghijklmno", start_ms);

  CHECK(task.m_begin_at_ms == start_ms);
  CHECK_FALSE(task.m_answered);
  CHECK(task.m_name == "abcdefghij");
}

TEST_CASE("The task name counts Unicode scalars, not bytes", "[openai][selection][session]") {
  session_store store;

  // Ten two-byte scalars: cutting at ten bytes would split the fifth one.
  auto const task = store.get_or_create("s", 0, "ééééééééééé", start_ms);

  CHECK(task.m_name == "éééééééééé");
}

TEST_CASE("An answered task stays answered across hits", "[openai][selection][session]") {
  session_store store;

  auto const first = store.get_or_create("s", 1, "hello", start_ms);
  store.mark_answered(first.m_id);
  auto const second = store.get_or_create("s", 1, "hello", start_ms + 5);

  CHECK(second.m_answered);
  CHECK(second.m_id == first.m_id);
}

TEST_CASE("Answering an unknown or empty task changes nothing", "[openai][selection][session]") {
  session_store store;

  auto const task = store.get_or_create("s", 1, "hello", start_ms);
  store.mark_answered("");
  store.mark_answered("not-a-task");

  CHECK_FALSE(store.get_or_create("s", 1, "hello", start_ms).m_answered);
  CHECK(store.size() == 1);
  CHECK(task.m_id != "");
}

TEST_CASE("A task expires after two idle hours", "[openai][selection][session]") {
  session_store store;

  auto const first = store.get_or_create("s", 1, "hello", start_ms);
  auto const stale = store.get_or_create("s", 1, "hello", start_ms + two_hours_ms + 1);

  CHECK(stale.m_id != first.m_id);
  CHECK(store.size() == 1);
}

TEST_CASE("A hit keeps the task alive past the idle window", "[openai][selection][session]") {
  session_store store;

  auto const first = store.get_or_create("s", 1, "hello", start_ms);
  // Only the refresh matters here: the hit has to age the task without replacing it.
  (void)store.get_or_create("s", 1, "hello", start_ms + hour_ms);
  auto const later = store.get_or_create("s", 1, "hello", start_ms + hour_ms + hour_ms - 1);

  CHECK(later.m_id == first.m_id);
}

TEST_CASE("A full store evicts the least recently used task", "[openai][selection][session]") {
  session_store store{2, two_hours_ms};

  auto const first = store.get_or_create("s", 0, "one", start_ms);
  // The results below are discarded on purpose: these calls fill the store to its capacity of two
  // and fix the order the eviction has to follow, and nothing else.
  (void)store.get_or_create("s", 1, "two", start_ms + 10);
  // Touching the first turn makes the second one the oldest entry.
  (void)store.get_or_create("s", 0, "one", start_ms + 20);
  (void)store.get_or_create("s", 2, "three", start_ms + 30);

  CHECK(store.size() == 2);
  CHECK(store.get_or_create("s", 0, "one", start_ms + 40).m_id == first.m_id);
  CHECK(store.get_or_create("s", 1, "two", start_ms + 50).m_id != first.m_id);
}

TEST_CASE("A zero capacity store still keeps one task", "[openai][selection][session]") {
  session_store store{0};

  auto const task = store.get_or_create("s", 0, "one", start_ms);

  CHECK(store.size() == 1);
  CHECK_FALSE(task.m_id.empty());
}
