#include "server/endpoints/health.hpp"

#include <string>

namespace qoderz::server::endpoints {

std::string health_json() {
  return R"({"status":"ok"})";
}

} // namespace qoderz::server::endpoints
