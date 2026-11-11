#include "server/router.hpp"

#include "server/error_writer.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace qoderz::server {
namespace {

constexpr std::string_view health_path = "/healthz";
constexpr std::string_view models_path = "/v1/models";
constexpr std::string_view chat_path = "/v1/chat/completions";

route_match method_not_allowed(std::string_view allow) {
  return route_match{.m_kind = route_kind::wrong_method, .m_allow = std::string{allow}};
}

} // namespace

std::string_view path_of(std::string_view target) noexcept {
  auto const query = target.find('?');
  return query == std::string_view::npos ? target : target.substr(0, query);
}

bool requires_authentication(std::string_view target) noexcept {
  auto const path = path_of(target);
  return path == models_path || path == chat_path;
}

route_match match_route(std::string_view method, std::string_view target) {
  auto const path = path_of(target);
  if (path == health_path) {
    if (method == "GET") {
      return route_match{.m_kind = route_kind::health};
    }
    return method_not_allowed("GET");
  }
  if (path == models_path) {
    if (method == "GET") {
      return route_match{.m_kind = route_kind::models};
    }
    return method_not_allowed("GET");
  }
  if (path == chat_path) {
    if (method == "POST") {
      return route_match{.m_kind = route_kind::chat};
    }
    return method_not_allowed("POST");
  }
  return route_match{};
}

error not_found_error() {
  return client_status_error(404, std::string{unknown_endpoint_message});
}

error wrong_method_error(std::string_view allow) {
  std::string message = "method not allowed; use ";
  message.append(allow);
  return client_status_error(405, std::move(message));
}

} // namespace qoderz::server
