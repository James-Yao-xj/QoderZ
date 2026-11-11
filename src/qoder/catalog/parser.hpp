#pragma once

#include "common/json.hpp"
#include "qoder/catalog/model.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::qoder::catalog {

/// The first of `names` that holds a JSON string, trimmed; an empty string when none does.
///
/// The model listing spells several fields in both camelCase and snake_case, so every reader takes
/// a name list and returns the first usable value instead of the caller probing field by field.
[[nodiscard]] std::string read_text(json const& object,
                                    std::initializer_list<std::string_view> names);

/// Whether the first present field of `names` is truthy: `true`, the number `1`, `"1"` or `"true"`.
///
/// A missing field is false, and the first *present* field decides — a later field never overrides
/// an explicit false, which is what lets `auto` and disabled models be filtered reliably.
[[nodiscard]] bool is_truthy(json const& object, std::initializer_list<std::string_view> names);

/// The first of `names` that holds a positive integer that fits in `int`, otherwise 0.
[[nodiscard]] int read_positive_int(json const& object,
                                    std::initializer_list<std::string_view> names);

/// The first of `names` that holds a JSON number, otherwise `std::nullopt`.
///
/// The optional matters for pricing: an absent price and a price of zero select different `free`
/// rules, so callers cannot collapse the two cases into a default value.
[[nodiscard]] std::optional<double> find_number(json const& object,
                                                std::initializer_list<std::string_view> names);

/// Flattens one model object of the listing; a non-object yields an empty descriptor.
///
/// The window chain, the thinking flags, the effort order and the price rules are the ones in
/// docs/protocol/qoder-auth.md §6. `m_raw_json` is `raw.dump()`, so whitespace is not preserved but
/// every field is.
[[nodiscard]] model_info parse_model(json const& raw);

} // namespace qoderz::qoder::catalog
