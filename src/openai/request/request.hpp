#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/request/types.hpp"
#include "openai/selection/session.hpp"
#include "qoder/catalog/model.hpp"

#include <optional>
#include <string_view>

namespace qoderz::openai {

/// Translates one Chat Completions body per `docs/protocol/openai-compat.md` §1, §2 and §4.
///
/// Only a missing or non-array `messages` is an error. Every other field is read leniently, because
/// a client that also sends `temperature` should still get an answer instead of a 400.
///
/// `model` is already resolved by the endpoint. `time` is the only source of "now", so a test can
/// pin the task timestamps.
[[nodiscard]] result<request_context>
prepare_request(json const& body, std::optional<std::string_view> session_header,
                qoder::catalog::model_info const& model, selection::session_store& sessions,
                clock& time);

} // namespace qoderz::openai
