#include "qoder/chat/request.hpp"

#include "common/strings.hpp"
#include "protocol/body_codec.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::qoder::chat {
namespace {

constexpr std::string_view chat_path = "/algo/api/v2/service/pro/sse/agent_chat_generation"
                                       "?FetchKeys=llm_model_result&AgentId=agent_common&Encode=1";
constexpr std::string_view event_stream_accept = "text/event-stream";

/// The identity COSY encrypts; the display name falls back to the account name so a fresh record
/// still signs with something readable.
protocol::cosy_identity identity_of(config::account_record const& account) {
  return protocol::cosy_identity{
      .m_uid = account.m_uid,
      .m_name = account.m_display_name.empty() ? account.m_name : account.m_display_name,
      .m_email = account.m_email,
      .m_access_token = account.m_access_token,
      .m_machine_id = account.m_machine_id,
      .m_machine_os = protocol::machine_os(),
  };
}

} // namespace

result<signed_request> build_signed_request(upstream_request const& request,
                                            protocol::rsa_key_wrapper const& wrapper,
                                            std::int64_t unix_seconds) {
  auto wire_body = protocol::body_encode(request.m_body_json);

  std::string url{request.m_site.m_api_base};
  url.append(chat_path);

  auto material = protocol::create_cosy_headers(url, identity_of(request.m_account), wire_body,
                                                unix_seconds, wrapper);
  if (!material) {
    return std::unexpected(std::move(material.error()));
  }

  signed_request built;
  built.m_wire_body = std::move(wire_body);
  built.m_headers.reserve(material->m_headers.size() + 3);
  for (auto const& header : material->m_headers) {
    // COSY signs an `application/json` request; the stream wants the event-stream media type, and
    // one header with the streaming value is what goes on the wire.
    auto value = header.m_value;
    if (strings::equals_ignore_case(header.m_name, "Accept")) {
      value = std::string{event_stream_accept};
    }
    built.m_headers.push_back(
        net::http_header{.m_name = header.m_name, .m_value = std::move(value)});
  }
  built.m_headers.push_back(net::http_header{.m_name = "Cache-Control", .m_value = "no-cache"});
  built.m_headers.push_back(
      net::http_header{.m_name = "X-Model-Key", .m_value = request.m_model_key});
  if (!request.m_model_source.empty()) {
    built.m_headers.push_back(
        net::http_header{.m_name = "X-Model-Source", .m_value = request.m_model_source});
  }
  built.m_url = std::move(url);
  return built;
}

} // namespace qoderz::qoder::chat
