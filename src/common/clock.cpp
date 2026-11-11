#include "common/clock.hpp"

#include <chrono>

namespace qoderz {

std::int64_t system_clock::now_ms() const {
  auto const since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
}

} // namespace qoderz
