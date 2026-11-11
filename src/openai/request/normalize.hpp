#pragma once

#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/types.hpp"

#include <string>
#include <vector>

namespace qoderz::openai {

struct normalize_result {
  std::vector<normalized_message> m_messages;
  std::string m_first_user_text;
  std::string m_last_user_text;
  /// Index in the raw `messages` array, ignored entries included; -1 when there is no user turn.
  int m_last_user_index = -1;
};

/// Reads `messages` into the four Qoder roles, as `docs/protocol/openai-compat.md` §3 describes.
///
/// `system` and `developer` entries are dropped — they already became the system text. Tool results
/// pair with the immediately preceding assistant turn only, and images a tool returned move into
/// their own following user turns. Only a non-array `messages` fails: an entry carrying an unknown
/// part type is read leniently, so the client still gets an answer.
[[nodiscard]] result<normalize_result> normalize_messages(json const& messages);

/// The plain text of a content value: a string as-is, or an object's `text` when its `type` is
/// empty, `text` or `input_text`; an array joins its non-empty parts with a newline. Text is never
/// trimmed, because whitespace is part of the prompt.
[[nodiscard]] std::string read_text_content(json const& content);

} // namespace qoderz::openai
