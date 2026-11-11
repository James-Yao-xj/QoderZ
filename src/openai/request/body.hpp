#pragma once

#include "common/error.hpp"
#include "openai/request/types.hpp"

#include <string>

namespace qoderz::openai {

struct built_body {
  std::string m_json;
  int m_context_length = 0;
};

/// Serialises the request in the field order `docs/protocol/openai-compat.md` §6 fixes.
///
/// `parameters` trails the payload because `context_length` depends on the serialised message and
/// tool sizes, so those must be measured first — callers cannot reorder the fields afterwards.
[[nodiscard]] result<built_body> build_qoder_body(request_context const& context);

} // namespace qoderz::openai
