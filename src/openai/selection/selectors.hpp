#pragma once

#include "qoder/catalog/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace qoderz::openai::selection {

struct effort_selection {
  bool m_enable_thinking = true;
  /// Empty when the model has no ladder or the request leaves the default alone; the body writer
  /// omits the field in both cases.
  std::string m_effort;
};

/// Picks the effort per `docs/protocol/openai-compat.md` §5.1: the input is trimmed and lowered,
/// `ultra` means `max`, an unknown name falls back to the model default, a tie picks the higher
/// rank, and the default itself is never validated against the ladder.
[[nodiscard]] effort_selection select_effort(qoder::catalog::model_info const& model,
                                             std::string_view requested);

/// The context window to report for a request of this size, per §5.2. The byte counts are of the
/// already serialised JSON, so escaping changes the answer; a request without tools counts two
/// bytes, the `"[]"` it would have written.
[[nodiscard]] int select_window(qoder::catalog::model_info const& model, std::size_t messages_bytes,
                                std::size_t tools_bytes, int max_tokens);

} // namespace qoderz::openai::selection
