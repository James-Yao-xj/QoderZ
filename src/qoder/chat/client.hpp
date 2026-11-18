#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "protocol/cosy_signer.hpp"
#include "qoder/chat/request.hpp"
#include "qoder/chat/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>

namespace qoderz::qoder::chat {

/// Opens upstream chat responses, retrying only while the account is queued.
///
/// A request is signed once and every retry resends those exact bytes and headers, `Cosy-Date`
/// included, because the queue is upstream's answer to this request rather than a new one. Only
/// `error_kind::busy` is retried; auth and quota come back immediately so the caller can swap the
/// account, and a transport failure is never retried.
class chat_client {
public:
  chat_client(net::http_client& http, protocol::rsa_key_wrapper const& wrapper, clock& time,
              retry_options options = {});

  /// Takes ownership of the request and returns a stream positioned before its first event.
  /// Move an owned request here to avoid copying its plaintext body.
  ///
  /// The first event is read before the stream is handed over, because it is the last moment an
  /// account can still be swapped; it is put back so the caller sees the whole response. A response
  /// that ends without any event yields a stream that is already finished, which the caller answers
  /// with an empty completion.
  boost::asio::awaitable<result<std::unique_ptr<chat_stream>>> open(upstream_request request);

private:
  boost::asio::awaitable<result<void>> wait(std::chrono::milliseconds delay);

  net::http_client& m_http;
  protocol::rsa_key_wrapper const& m_wrapper;
  clock& m_time;
  retry_options m_options;
};

} // namespace qoderz::qoder::chat
