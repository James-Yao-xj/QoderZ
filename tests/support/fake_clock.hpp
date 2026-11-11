#pragma once

#include "common/clock.hpp"

#include <chrono>
#include <cstdint>

namespace qoderz::test {

/// A clock that stands still until a test moves it, so an expected `expires_at` is exact
/// arithmetic instead of "somewhere after now".
class fake_clock final : public qoderz::clock {
public:
  explicit fake_clock(std::int64_t now_ms = 1'760'000'000'000) : m_now_ms{now_ms} {}

  [[nodiscard]] std::int64_t now_ms() const override { return m_now_ms; }

  void set_now_ms(std::int64_t now_ms) noexcept { m_now_ms = now_ms; }

  void advance(std::chrono::milliseconds delta) noexcept { m_now_ms += delta.count(); }

private:
  std::int64_t m_now_ms;
};

} // namespace qoderz::test
