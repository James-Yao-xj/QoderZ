#include "qoder/catalog/listing.hpp"

#include "qoder/catalog/parser.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qoderz::qoder::catalog {
namespace {

using json_list = std::vector<json>;

/// A clone of `value` when it is an array; a missing field or another kind is `nullopt`.
std::optional<json_list> array_clone(json const& value) {
  if (!value.is_array()) {
    return std::nullopt;
  }
  json_list items;
  items.reserve(value.size());
  for (auto const& item : value) {
    items.push_back(item);
  }
  return items;
}

/// key -> position, first occurrence wins, matching the order the merge walks the list in.
std::map<std::string, std::size_t, std::less<>> index_of(json_list const& items) {
  std::map<std::string, std::size_t, std::less<>> index;
  for (std::size_t position = 0; position < items.size(); ++position) {
    auto const key = read_text(items[position], {"key"});
    if (!key.empty()) {
      index.try_emplace(key, position);
    }
  }
  return index;
}

} // namespace

std::vector<json> merge_listing(json const& app, json const& chat) {
  auto const app_items = array_clone(app);
  auto const chat_items = array_clone(chat);

  if (!app_items.has_value()) {
    return chat_items.value_or(json_list{});
  }

  json_list merged = *app_items;
  if (!chat_items.has_value()) {
    return merged;
  }

  auto index = index_of(merged);
  // -1 means "no anchor yet": a new chat key goes to the end of the list.
  std::ptrdiff_t anchor = -1;
  for (auto const& item : *chat_items) {
    if (!item.is_object()) {
      continue;
    }
    auto const key = read_text(item, {"key"});
    if (!key.empty()) {
      if (auto const found = index.find(key); found != index.end()) {
        anchor = static_cast<std::ptrdiff_t>(found->second);
        continue;
      }
    }

    auto const position = anchor >= 0 ? static_cast<std::size_t>(anchor) + 1 : merged.size();
    merged.insert(merged.begin() + static_cast<std::ptrdiff_t>(position), item);
    index = index_of(merged);
    anchor = static_cast<std::ptrdiff_t>(position);
  }

  return merged;
}

std::vector<model_info> parse_models(std::vector<json> const& merged) {
  std::vector<model_info> models;
  models.reserve(merged.size());
  std::unordered_set<std::string> seen;

  for (auto const& raw : merged) {
    auto const key = read_text(raw, {"key"});
    if (key.empty() || key == "auto" || key == "default") {
      continue;
    }
    if (!is_truthy(raw, {"enable"})) {
      continue;
    }
    if (!seen.insert(key).second) {
      continue;
    }
    models.push_back(parse_model(raw));
  }

  return models;
}

} // namespace qoderz::qoder::catalog
