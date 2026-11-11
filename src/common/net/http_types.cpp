#include "common/net/http_types.hpp"

#include "common/strings.hpp"

namespace qoderz::net {

std::string const* header_value(std::vector<http_header> const& headers,
                                std::string_view name) noexcept {
  for (auto const& header : headers) {
    if (strings::equals_ignore_case(header.m_name, name)) {
      return &header.m_value;
    }
  }
  return nullptr;
}

} // namespace qoderz::net
