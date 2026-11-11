#pragma once

#include "common/json.hpp"
#include "qoder/catalog/model.hpp"

#include <string_view>
#include <vector>

namespace qoderz::qoder::catalog {

/// Interleaves the `app` and `chat` arrays of a listing into the order the site presents.
///
/// Both arguments are the array values themselves (the caller reads the `app` and `chat` members),
/// so a missing field is passed as an empty `json` and counts as absent.
///
/// `app` is the backbone. A chat entry whose key is already in `app` only moves the insertion
/// anchor — the app version wins, because its fields are the ones a chat request must echo. A new
/// key is inserted right after the anchor, so chat entries that sit between two app entries keep
/// their relative position; an anchor of -1 appends to the end. An absent or non-array `app` falls
/// back to a clone of `chat`, and an absent or non-array `chat` leaves the app clone untouched.
[[nodiscard]] std::vector<json> merge_listing(json const& app, json const& chat);

/// The usable models of a merged listing, in listing order.
///
/// A model needs a key, a key that is neither `auto` nor `default`, a truthy `enable`, and it must
/// be the first entry carrying that key. Everything else is dropped before parsing, so a disabled
/// duplicate cannot shadow the entry that is actually offered.
[[nodiscard]] std::vector<model_info> parse_models(std::vector<json> const& merged);

/// The built-in model list of a site, used when the account's listing comes back empty.
///
/// The data lives in `fallback.cpp`; an unknown site falls back to the international list, because
/// a missing model list must never turn into "this site has no models".
[[nodiscard]] std::vector<model_info> const& fallback_models(std::string_view site_id);

} // namespace qoderz::qoder::catalog
