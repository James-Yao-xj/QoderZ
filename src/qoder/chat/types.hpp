#pragma once

#include "common/error.hpp"
#include "config/accounts.hpp"
#include "config/site.hpp"
#include "protocol/sse/chat_event.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <optional>
#include <string>

namespace qoderz::qoder::chat {

/// One chat request as the layer above hands it over: the resolved site and account, the plaintext
/// Qoder body and the catalog keys the upstream headers need. Building the URL, encoding the body
/// and signing are the client's job, so this type never carries a credential-derived string.
struct upstream_request {
  config::resolved_site m_site;
  config::account_record m_account;
  /// The plaintext Qoder request JSON; `body_encode` runs inside `build_signed_request`.
  std::string m_body_json;
  /// The catalog key, which is what `X-Model-Key` carries — not the client's model string.
  std::string m_model_key;
  /// The catalog source; empty omits `X-Model-Source`.
  std::string m_model_source;
};

/// Events of one upstream response, in order.
///
/// `next()` yields `nullopt` once the stream ended cleanly, and an `error` for a transport or
/// parse failure. A stream that carried an upstream Error frame ends with that event and yields
/// `nullopt` afterwards; it never synthesises a Stop for it. Destroying the stream drops the
/// connection, which is how a cancelled client stops reading.
class chat_stream {
public:
  chat_stream() = default;
  chat_stream(chat_stream const&) = delete;
  chat_stream& operator=(chat_stream const&) = delete;
  virtual ~chat_stream() = default;

  virtual boost::asio::awaitable<result<std::optional<protocol::chat_event>>> next() = 0;
};

/// The queue-retry budget of `docs/protocol/qoder-chat.md` §5.
struct retry_options {
  std::chrono::milliseconds m_total_budget{4 * 60 * 1000};
  int m_max_attempts = 11;
  std::chrono::milliseconds m_min_delay{500};
  std::chrono::milliseconds m_max_delay{30 * 1000};
};

} // namespace qoderz::qoder::chat
