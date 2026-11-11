#include "qoder/catalog/listing.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::qoder::catalog {
namespace {

/// The effort ladders of the built-in list. They mirror the upstream listing closely enough that a
/// caller can pick a level before the first successful fetch.
constexpr std::array<std::string_view, 5> full_efforts{"low", "medium", "high", "xhigh", "max"};
constexpr std::array<std::string_view, 3> qwen_efforts{"low", "medium", "xhigh"};
constexpr std::array<std::string_view, 3> low_high_max_efforts{"low", "high", "max"};
constexpr std::array<std::string_view, 2> high_max_efforts{"high", "max"};

struct fallback_row {
  std::string_view m_key;
  std::string_view m_name;
  int m_context;
  std::span<std::string_view const> m_efforts;
  bool m_images = true;
};

constexpr std::array<fallback_row, 17> qoder_rows{{
    {"ultimate", "Ultimate", 1'000'000, full_efforts},
    {"fast", "Fast", 1'000'000, {}},
    {"performance", "Performance", 1'000'000, full_efforts},
    {"efficient", "Efficient", 200'000, {}},
    {"smodel", "Sonus", 180'000, full_efforts},
    {"cmodel", "Cantus", 180'000, full_efforts},
    {"qmodel_38max", "Qwen3.8-Max", 180'000, qwen_efforts},
    {"qfmodel", "Qwen3.8-Flash", 180'000, qwen_efforts},
    {"qmodel_latest", "Qwen3.7-Max", 1'000'000, {}},
    {"qmodel", "Qwen3.7-Plus", 1'000'000, {}},
    {"kmodel_latest", "Kimi-K3", 180'000, low_high_max_efforts},
    {"kmodel", "Kimi-K2.8-Preview", 0, low_high_max_efforts},
    {"gmodel", "GLM-5.3", 180'000, low_high_max_efforts},
    {"gfmodel", "GLM-5.3-Flash", 1'000'000, high_max_efforts},
    {"dmodel", "DeepSeek-V4-Pro", 1'000'000, high_max_efforts},
    {"dfmodel", "DeepSeek-Flash", 1'000'000, low_high_max_efforts},
    {"mmodel", "MiniMax-M3", 180'000, {}},
}};

/// The CN site offers only a subset of the international levels, and a model the site does not know
/// is one the user can pick and then be told is `unknown or disabled model`. That is worse than a
/// short list, so the CN list keeps only the three levels the site really has, in listing order.
constexpr std::array<std::string_view, 3> chinese_keys{"ultimate", "performance", "efficient"};

/// Whether the CN site offers `key`.
constexpr bool is_chinese_key(std::string_view key) {
  for (auto const known : chinese_keys) {
    if (known == key) {
      return true;
    }
  }
  return false;
}

/// `lite` is the one CN-only entry, and the one entry without image support.
constexpr fallback_row lite_row{"lite", "Lite", 0, {}, false};

/// One built-in entry. `default_window` follows `context`, as the upstream listing does for a model
/// whose only window is its full context; `raw_json` stays `{}` because there is no listing object
/// to echo back.
model_info make_model(fallback_row const& row) {
  model_info model;
  model.m_key = std::string{row.m_key};
  model.m_name = std::string{row.m_name};
  model.m_context = row.m_context;
  model.m_default_window = row.m_context;
  model.m_images = row.m_images;
  model.m_thinks = !row.m_efforts.empty();
  model.m_efforts.reserve(row.m_efforts.size());
  for (auto const level : row.m_efforts) {
    model.m_efforts.emplace_back(level);
  }
  model.m_raw_json = "{}";
  return model;
}

std::vector<model_info> build_international() {
  std::vector<model_info> models;
  models.reserve(qoder_rows.size());
  for (auto const& row : qoder_rows) {
    models.push_back(make_model(row));
  }
  return models;
}

/// The CN list is the shared levels of `qoder_rows` plus the CN-only `lite`, in `qoder_rows` order.
std::vector<model_info> build_china() {
  std::vector<model_info> models;
  models.reserve(chinese_keys.size() + 1);
  for (auto const& row : qoder_rows) {
    if (is_chinese_key(row.m_key)) {
      models.push_back(make_model(row));
    }
  }
  models.push_back(make_model(lite_row));
  return models;
}

} // namespace

std::vector<model_info> const& fallback_models(std::string_view site_id) {
  static std::vector<model_info> const international = build_international();
  static std::vector<model_info> const china = build_china();
  return site_id == "qoder-cn" ? china : international;
}

} // namespace qoderz::qoder::catalog
