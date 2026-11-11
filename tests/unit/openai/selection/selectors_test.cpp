#include "openai/selection/selectors.hpp"
#include "qoder/catalog/model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace {

using qoderz::openai::selection::effort_selection;
using qoderz::openai::selection::select_effort;
using qoderz::openai::selection::select_window;
using qoderz::qoder::catalog::model_info;

model_info model_with(std::vector<std::string> efforts, std::string default_effort,
                      bool thinks = true, bool always_thinks = false) {
  model_info model;
  model.m_key = "performance";
  model.m_efforts = std::move(efforts);
  model.m_default_effort = std::move(default_effort);
  model.m_thinks = thinks;
  model.m_always_thinks = always_thinks;
  return model;
}

model_info window_model(std::vector<int> windows, int default_window, int context) {
  model_info model;
  model.m_key = "performance";
  model.m_windows = std::move(windows);
  model.m_default_window = default_window;
  model.m_context = context;
  return model;
}

} // namespace

TEST_CASE("A model without thinking disables it", "[openai][selection][effort]") {
  auto const selection = select_effort(model_with({"low", "high"}, {}, false), "high");

  CHECK_FALSE(selection.m_enable_thinking);
  CHECK(selection.m_effort.empty());
}

TEST_CASE("None disables thinking only when the model can switch it off",
          "[openai][selection][effort]") {
  auto const switchable = select_effort(model_with({"low", "high"}, {}, true, false), "none");
  CHECK_FALSE(switchable.m_enable_thinking);
  CHECK(switchable.m_effort.empty());

  auto const forced = select_effort(model_with({"low", "high"}, {}, true, true), "none");
  CHECK(forced.m_enable_thinking);
  CHECK(forced.m_effort == "low");
}

TEST_CASE("A model without an effort ladder still enables thinking",
          "[openai][selection][effort]") {
  auto const selection = select_effort(model_with({}, {}, true, false), "high");

  CHECK(selection.m_enable_thinking);
  CHECK(selection.m_effort.empty());
}

TEST_CASE("An absent request uses the model default verbatim", "[openai][selection][effort]") {
  CHECK(select_effort(model_with({"low", "medium", "high"}, "medium"), "").m_effort == "medium");
  CHECK(select_effort(model_with({"low", "medium", "high"}, ""), "").m_effort.empty());
  // The default is never checked against the ladder; a listing may name a level it does not offer.
  CHECK(select_effort(model_with({"low", "high"}, "turbo"), " ").m_effort == "turbo");
}

TEST_CASE("A requested effort snaps to the nearest supported level",
          "[openai][selection][effort]") {
  auto const ladder = model_with({"low", "medium", "high"}, "medium");

  CHECK(select_effort(ladder, "minimal").m_effort == "low");
  CHECK(select_effort(ladder, "low").m_effort == "low");
  CHECK(select_effort(ladder, "medium").m_effort == "medium");
  CHECK(select_effort(ladder, "high").m_effort == "high");
  CHECK(select_effort(ladder, "xhigh").m_effort == "high");
  CHECK(select_effort(ladder, "max").m_effort == "high");
}

TEST_CASE("A tie between two levels picks the higher one", "[openai][selection][effort]") {
  CHECK(select_effort(model_with({"low", "high"}, {}), "medium").m_effort == "high");
}

TEST_CASE("Ultra means max and the input is trimmed and lowered", "[openai][selection][effort]") {
  auto const ladder = model_with({"low", "medium", "high", "max"}, {});

  CHECK(select_effort(ladder, " ultra ").m_effort == "max");
  CHECK(select_effort(ladder, "HIGH").m_effort == "high");
}

TEST_CASE("An unknown effort falls back to the model default", "[openai][selection][effort]") {
  CHECK(select_effort(model_with({"low", "high"}, "low"), "turbo").m_effort == "low");
  CHECK(select_effort(model_with({"low", "high"}, {}), "turbo").m_effort.empty());
}

TEST_CASE("Levels outside the ladder are skipped when picking", "[openai][selection][effort]") {
  auto const ladder = model_with({"turbo", "high"}, {}, true, true);

  CHECK(select_effort(ladder, "low").m_effort == "high");
  CHECK(select_effort(ladder, "none").m_effort == "high");
}

TEST_CASE("A ladder without a recognisable level keeps its first entry",
          "[openai][selection][effort]") {
  CHECK(select_effort(model_with({"turbo", "fast"}, {}, true, true), "none").m_effort == "turbo");
  CHECK(select_effort(model_with({"turbo", "fast"}, {}), "low").m_effort == "turbo");
}

TEST_CASE("The default window is used while the prompt fits", "[openai][selection][window]") {
  auto const model = window_model({200'000, 1'000'000}, 200'000, 1'000'000);

  CHECK(select_window(model, 3000, 0, 32'000) == 200'000);
}

TEST_CASE("Max tokens count toward the required window", "[openai][selection][window]") {
  auto const model = window_model({200'000, 1'000'000}, 200'000, 1'000'000);

  CHECK(select_window(model, 3000, 0, 250'000) == 1'000'000);
}

TEST_CASE("Tool bytes count toward the required window", "[openai][selection][window]") {
  auto const model = window_model({200'000, 1'000'000}, 200'000, 1'000'000);

  CHECK(select_window(model, 3000, 600'000, 32'000) == 1'000'000);
}

TEST_CASE("The smallest window that fits is chosen", "[openai][selection][window]") {
  auto const model = window_model({100'000, 300'000, 500'000}, 100'000, 500'000);

  CHECK(select_window(model, 600'000, 0, 32'000) == 300'000);
}

TEST_CASE("The largest window is chosen when nothing fits", "[openai][selection][window]") {
  auto const model = window_model({100'000, 200'000}, 100'000, 200'000);

  CHECK(select_window(model, 3'000'000, 0, 32'000) == 200'000);
}

TEST_CASE("A zero base falls back to the window list", "[openai][selection][window]") {
  auto const model = window_model({100'000}, 0, 0);

  CHECK(select_window(model, 3000, 0, 32'000) == 100'000);
}

TEST_CASE("A model without windows reports its context", "[openai][selection][window]") {
  CHECK(select_window(window_model({}, 0, 500'000), 10'000, 0, 32'000) == 500'000);
}
