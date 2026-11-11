#pragma once

#include "common/error.hpp"
#include "common/net/http_types.hpp"
#include "protocol/cosy_signer.hpp"
#include "qoder/chat/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qoderz::qoder::chat {

/// Everything one POST needs, signed once so a queue retry can resend the identical bytes.
struct signed_request {
  std::string m_url;
  std::vector<net::http_header> m_headers;
  std::string m_wire_body;
};

/// Builds the URL, the BodyCodec-encoded body and the headers of one `agent_chat_generation` call.
///
/// The body is encoded before signing, because COSY signs the wire bytes and the path without
/// `/algo`; `unix_seconds` goes into `Cosy-Date` and the signature alike, so a retry that reuses
/// the result keeps both stable. An account without a uid or machine id is
/// `error_kind::configuration`.
[[nodiscard]] result<signed_request> build_signed_request(upstream_request const& request,
                                                          protocol::rsa_key_wrapper const& wrapper,
                                                          std::int64_t unix_seconds);

} // namespace qoderz::qoder::chat
