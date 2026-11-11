#include "qoder/catalog/parser.hpp"

#include "common/strings.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::qoder::catalog {
namespace {

/// The canonical effort ladder. Anything outside it keeps its listing order and is appended, so a
/// level the site invents later stays selectable instead of silently disappearing.
constexpr std::array<std::string_view, 6> effort_order{"minimal", "low",   "medium",
                                                       "high",    "xhigh", "max"};

bool contains_text(std::vector<std::string> const& values, std::string_view candidate) {
  return std::any_of(values.begin(), values.end(), [candidate](std::string const& value) {
    return strings::equals_ignore_case(value, candidate);
  });
}

void append_unique(std::vector<std::string>& values, std::string_view level) {
  if (level.empty() || contains_text(values, level)) {
    return;
  }
  values.emplace_back(level);
}

std::vector<std::string> sort_efforts(std::vector<std::string> levels) {
  if (levels.size() < 2) {
    return levels;
  }
  std::vector<std::string> sorted;
  sorted.reserve(levels.size());
  for (auto const known : effort_order) {
    for (auto const& candidate : levels) {
      if (strings::equals_ignore_case(candidate, known)) {
        sorted.push_back(candidate);
      }
    }
  }
  for (auto const& candidate : levels) {
    if (!contains_text(sorted, candidate)) {
      sorted.push_back(candidate);
    }
  }
  return sorted;
}

/// Reads `enabled.efforts`, a map of level to config object.
void read_effort_map(json const& map, std::vector<std::string>& levels, std::string& preferred) {
  for (auto const& [name, entry] : map.items()) {
    auto const level = std::string{strings::trim(name)};
    if (level.empty()) {
      continue;
    }
    append_unique(levels, level);
    if (preferred.empty() && is_truthy(entry, {"is_default"})) {
      preferred = level;
    }
  }
}

/// The `reasoning_efforts` array, whose entries are either a bare name or an object naming one.
void read_reasoning_efforts(json const& raw, std::vector<std::string>& levels,
                            std::string& preferred) {
  auto const array = raw.find("reasoning_efforts");
  if (array == raw.end() || !array->is_array()) {
    return;
  }
  for (auto const& entry : *array) {
    std::string level;
    if (entry.is_string()) {
      level = std::string{strings::trim(entry.get_ref<std::string const&>())};
    } else if (entry.is_object()) {
      level = read_text(entry, {"effort", "name", "key"});
    }
    if (level.empty()) {
      continue;
    }
    append_unique(levels, level);
    if (preferred.empty() && entry.is_object() && is_truthy(entry, {"is_default"})) {
      preferred = level;
    }
  }
}

/// Fills the thinking flags and efforts; `raw` must be an object.
void read_thinking(json const& raw, model_info& model) {
  std::vector<std::string> levels;
  std::string preferred;

  auto const config = raw.find("thinking_config");
  if (config != raw.end() && config->is_object()) {
    auto const enabled = config->find("enabled");
    auto const disabled = config->find("disabled");
    bool const has_enabled = enabled != config->end() && !enabled->is_null();
    bool const has_disabled = disabled != config->end() && !disabled->is_null();

    model.m_thinks = has_enabled;
    model.m_always_thinks = has_enabled && !has_disabled;

    if (has_enabled && enabled->is_object()) {
      auto const efforts = enabled->find("efforts");
      if (efforts != enabled->end() && efforts->is_object()) {
        read_effort_map(*efforts, levels, preferred);
      }
    }
  } else {
    model.m_thinks = is_truthy(raw, {"is_reasoning"});
    model.m_always_thinks = false;
  }

  if (levels.empty()) {
    read_reasoning_efforts(raw, levels, preferred);
  }

  model.m_efforts = sort_efforts(std::move(levels));
  model.m_default_effort = std::move(preferred);
}

/// The windows a model accepts, plus the one the listing marks as the default.
struct window_choice {
  std::vector<int> m_windows;
  int m_default = 0;
};

void append_window(std::vector<int>& windows, int count) {
  if (count <= 0 || std::find(windows.begin(), windows.end(), count) != windows.end()) {
    return;
  }
  windows.push_back(count);
}

/// `available_context_windows` plus `default_context_window`, the flat shape older listings use.
window_choice read_available_windows(json const& raw) {
  window_choice choice;
  auto const available = raw.find("available_context_windows");
  if (available != raw.end() && available->is_array()) {
    for (auto const& entry : *available) {
      if (entry.is_number_integer()) {
        append_window(choice.m_windows, entry.get<int>());
      } else if (entry.is_number_unsigned()) {
        auto const value = entry.get<unsigned int>();
        if (value <= static_cast<unsigned int>(std::numeric_limits<int>::max())) {
          append_window(choice.m_windows, static_cast<int>(value));
        }
      }
    }
  }
  choice.m_default = read_positive_int(raw, {"default_context_window"});
  return choice;
}

window_choice read_windows(json const& raw) {
  window_choice choice;
  auto const config = raw.find("context_config");
  if (config != raw.end() && config->is_object()) {
    for (auto const& [name, entry] : config->items()) {
      (void)name;
      if (!entry.is_object()) {
        continue;
      }
      auto const count = read_positive_int(entry, {"token_count"});
      if (count == 0) {
        continue;
      }
      append_window(choice.m_windows, count);
      if (choice.m_default == 0 && is_truthy(entry, {"is_default"})) {
        choice.m_default = count;
      }
    }
  }

  if (choice.m_windows.empty()) {
    choice = read_available_windows(raw);
  }

  std::sort(choice.m_windows.begin(), choice.m_windows.end());
  return choice;
}

bool is_promotion_live(json const& promotion) {
  auto const active = promotion.find("active");
  if (active != promotion.end() && active->is_boolean() && !active->get<bool>()) {
    return false;
  }
  auto const enabled = promotion.find("enabled");
  return !(enabled != promotion.end() && enabled->is_boolean() && !enabled->get<bool>());
}

/// The promotion block, when the listing carries a live one; a switched-off block is not pricing.
std::optional<json> live_promotion(json const& raw) {
  auto const promotion = raw.find("promotion");
  if (promotion == raw.end() || !promotion->is_object() || !is_promotion_live(*promotion)) {
    return std::nullopt;
  }
  return *promotion;
}

void read_pricing(json const& raw, model_info& model) {
  auto const price = find_number(raw, {"price_factor", "priceFactor"});
  model.m_rate = price.value_or(0);

  double before = 0;
  double discount = 0;
  if (auto const promotion = live_promotion(raw); promotion.has_value()) {
    before =
        find_number(*promotion, {"before_promotion_price_factor", "beforePromotionPriceFactor"})
            .value_or(0);
    discount = find_number(*promotion, {"discount_factor", "discountFactor"}).value_or(0);
  }

  model.m_free =
      price.has_value() ? *price == 0 && before <= 0 : is_truthy(raw, {"is_free", "isFree"});
  model.m_rate_was = find_number(raw, {"original_price_factor", "originalPriceFactor"}).value_or(0);

  if (before > 0) {
    model.m_rate_was = before;
    if (model.m_rate == 0) {
      model.m_rate = before * discount;
    }
  }

  if (model.m_rate_was <= model.m_rate) {
    model.m_rate_was = 0;
  }
}

} // namespace

std::string read_text(json const& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object()) {
    return {};
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value == object.end() || !value->is_string()) {
      continue;
    }
    auto const text = std::string{strings::trim(value->get_ref<std::string const&>())};
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

bool is_truthy(json const& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object()) {
    return false;
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value == object.end()) {
      continue;
    }
    if (value->is_boolean()) {
      return value->get<bool>();
    }
    if (value->is_number()) {
      return value->get<double>() == 1.0;
    }
    if (value->is_string()) {
      auto const text = value->get_ref<std::string const&>();
      return text == "1" || strings::equals_ignore_case(text, "true");
    }
    return false;
  }
  return false;
}

int read_positive_int(json const& object, std::initializer_list<std::string_view> names) {
  if (!object.is_object()) {
    return 0;
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value == object.end()) {
      continue;
    }
    if (value->is_number_integer()) {
      auto const number = value->get<long long>();
      if (number > 0 && number <= std::numeric_limits<int>::max()) {
        return static_cast<int>(number);
      }
    } else if (value->is_number_unsigned()) {
      auto const number = value->get<unsigned long long>();
      if (number > 0 &&
          number <= static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        return static_cast<int>(number);
      }
    }
  }
  return 0;
}

std::optional<double> find_number(json const& object,
                                  std::initializer_list<std::string_view> names) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  for (auto const name : names) {
    auto const value = object.find(name);
    if (value != object.end() && value->is_number()) {
      return value->get<double>();
    }
  }
  return std::nullopt;
}

model_info parse_model(json const& raw) {
  model_info model;
  if (!raw.is_object()) {
    return model;
  }

  model.m_key = read_text(raw, {"key"});
  model.m_name = read_text(raw, {"display_name"});
  if (model.m_name.empty()) {
    model.m_name = model.m_key;
  }
  model.m_source = read_text(raw, {"source"});

  auto choice = read_windows(raw);
  model.m_windows = std::move(choice.m_windows);
  model.m_default_window = choice.m_default;
  model.m_context = model.m_windows.empty() ? read_positive_int(raw, {"max_input_tokens"})
                                            : model.m_windows.back();

  model.m_images = is_truthy(raw, {"is_vl"});
  read_thinking(raw, model);
  read_pricing(raw, model);

  model.m_raw_json = raw.dump();
  return model;
}

} // namespace qoderz::qoder::catalog
