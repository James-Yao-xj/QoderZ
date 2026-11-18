#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/normalize.hpp"
#include "openai/request/types.hpp"
#include "openai/selection/session.hpp"
#include "qoder/catalog/model.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::openai {

/// The share of a request that the body alone decides, read before any account or model is chosen.
///
/// §6.3 of docs/design/server.md keys session affinity by the derived session id, so the id has to
/// exist before candidate selection. The endpoint therefore reads the body once, hands
/// `m_session_id` to the pool, and only then prepares the request for the account that serves.
struct prepared_request {
  /// Normalized messages and last-turn text; first-user text is released after deriving the id.
  normalize_result m_dialog;
  std::string m_system_text;
  std::string m_session_id;
  std::string m_requested_effort;
  std::vector<tool_spec> m_tools;
  bool m_include_tools = false;
  /// Always filled by `read_request`: the body's limit, or the 32000 default.
  int m_max_tokens = 0;
};

/// Reads one Chat Completions body per `docs/protocol/openai-compat.md` §1, §2, §3 and §4.1.
///
/// Only a missing or non-array `messages` is an error. Every other field is read leniently, because
/// a client that also sends `temperature` should still get an answer instead of a 400.
[[nodiscard]] result<prepared_request> read_request(json const& body,
                                                    std::optional<std::string_view> session_header,
                                                    selection::session_store const& sessions);

/// Adds what depends on the chosen model — the §5 selectors — and picks the task of this turn per
/// §4.2. `time` is the only source of "now", so a test can pin the task timestamps.
[[nodiscard]] result<request_context> prepare_request(prepared_request const& prepared,
                                                      qoder::catalog::model_info const& model,
                                                      selection::session_store& sessions,
                                                      clock& time);

/// Consumes the large request payload while retaining metadata for account retries.
[[nodiscard]] result<request_context> prepare_request(prepared_request&& prepared,
                                                      qoder::catalog::model_info const& model,
                                                      selection::session_store& sessions,
                                                      clock& time);

/// Reads the body and prepares it for `model` in one step.
[[nodiscard]] result<request_context>
prepare_request(json const& body, std::optional<std::string_view> session_header,
                qoder::catalog::model_info const& model, selection::session_store& sessions,
                clock& time);
} // namespace qoderz::openai
