#pragma once

#include <cstdint>

namespace qoderz {

/// Wall-clock time in Unix milliseconds.
///
/// Only absolute wall-clock values (token expiry, record timestamps) go through it; durations such
/// as a poll budget stay on `std::chrono::steady_clock`, so a fake wall clock never makes a real
/// wait look expired.
class clock {
public:
  virtual ~clock() = default;

  [[nodiscard]] virtual std::int64_t now_ms() const = 0;
};

class system_clock final : public clock {
public:
  [[nodiscard]] std::int64_t now_ms() const override;
};

} // namespace qoderz
