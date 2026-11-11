#include "openai/selection/selectors.hpp"

#include "common/strings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qoderz::openai::selection {
namespace {

constexpr std::array<std::string_view, 6> effort_ranks{"minimal", "low",   "medium",
                                                       "high",    "xhigh", "max"};

constexpr std::int64_t envelope_overhead = 11;

std::string normalize_effort(std::string_view effort) {
  auto text = strings::ascii_lower_copy(strings::trim(effort));
  return text == "ultra" ? std::string{"max"} : text;
}

int effort_index(std::string_view effort) {
  for (std::size_t index = 0; index < effort_ranks.size(); ++index) {
    if (effort_ranks[index] == effort) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

int rank_distance(int lhs, int rhs) noexcept {
  return lhs > rhs ? lhs - rhs : rhs - lhs;
}

// Both pickers report a canonical ladder name rather than the model's own spelling, which is what
// the upstream accepts in `parameters`.
std::string lowest_effort(std::vector<std::string> const& efforts) {
  int selected = -1;
  for (auto const& effort : efforts) {
    auto const index = effort_index(normalize_effort(effort));
    if (index >= 0 && (selected < 0 || index < selected)) {
      selected = index;
    }
  }
  return selected < 0 ? efforts.front() : std::string{effort_ranks[selected]};
}

// An equal distance keeps the higher rank: that is the only tie-break that makes a `[low, high]`
// ladder answer `medium` deterministically as `high`.
std::string nearest_effort(std::vector<std::string> const& efforts, int target) {
  int selected = -1;
  int selected_distance = 0;
  for (auto const& effort : efforts) {
    auto const index = effort_index(normalize_effort(effort));
    if (index < 0) {
      continue;
    }
    auto const distance = rank_distance(index, target);
    if (selected >= 0 &&
        (distance > selected_distance || (distance == selected_distance && index <= selected))) {
      continue;
    }
    selected = index;
    selected_distance = distance;
  }
  return selected < 0 ? efforts.front() : std::string{effort_ranks[selected]};
}

// The model default is passed through unchecked: a listing may name a level its own ladder does not
// contain, and hiding that would silently change what the client asked for.
std::string model_default_effort(qoder::catalog::model_info const& model) {
  return normalize_effort(model.m_default_effort);
}

} // namespace

effort_selection select_effort(qoder::catalog::model_info const& model,
                               std::string_view requested) {
  auto const asked = normalize_effort(requested);
  if (!model.m_thinks || (asked == "none" && !model.m_always_thinks)) {
    return effort_selection{.m_enable_thinking = false, .m_effort = {}};
  }
  if (model.m_efforts.empty()) {
    return effort_selection{.m_enable_thinking = true, .m_effort = {}};
  }
  if (asked.empty()) {
    return effort_selection{.m_enable_thinking = true, .m_effort = model_default_effort(model)};
  }
  if (asked == "none") {
    return effort_selection{.m_enable_thinking = true, .m_effort = lowest_effort(model.m_efforts)};
  }

  auto const target = effort_index(asked);
  if (target < 0) {
    return effort_selection{.m_enable_thinking = true, .m_effort = model_default_effort(model)};
  }
  return effort_selection{.m_enable_thinking = true,
                          .m_effort = nearest_effort(model.m_efforts, target)};
}

int select_window(qoder::catalog::model_info const& model, std::size_t messages_bytes,
                  std::size_t tools_bytes, int max_tokens) {
  if (model.m_windows.empty()) {
    return model.m_context;
  }

  auto const length = static_cast<std::int64_t>(messages_bytes) +
                      static_cast<std::int64_t>(tools_bytes) + envelope_overhead;
  auto const tokens = static_cast<std::int64_t>(max_tokens > 0 ? max_tokens : 0);
  auto const need = (length + 2) / 3 + tokens;
  auto const base = model.m_default_window > 0 ? model.m_default_window : model.m_context;
  if (base > 0 && need <= base) {
    return base;
  }

  int smallest = 0;
  int largest = 0;
  for (auto const window : model.m_windows) {
    if (window > largest) {
      largest = window;
    }
    if (window >= need && (smallest == 0 || window < smallest)) {
      smallest = window;
    }
  }
  if (smallest > 0) {
    return smallest;
  }
  return largest > 0 ? largest : model.m_context;
}

} // namespace qoderz::openai::selection
