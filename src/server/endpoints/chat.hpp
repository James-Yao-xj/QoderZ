#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/json.hpp"
#include "openai/selection/session.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "qoder/chat/client.hpp"
#include "server/http/chat_handler.hpp"
#include "server/pool/account_pool.hpp"

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace qoderz::server::endpoints {

/// `/v1/chat/completions` of docs/design/server.md §5.
///
/// The handler holds no connection state, so one instance serves every connection of the process.
class chat_handler_impl final : public chat_handler {
public:
  chat_handler_impl(account_pool& pool, qoder::catalog::catalog_service& catalog,
                    qoder::auth::token_service& tokens, qoder::chat::chat_client& client,
                    openai::selection::session_store& sessions, clock& time, std::string site_id);

  chat_handler_impl(chat_handler_impl const&) = delete;
  chat_handler_impl(chat_handler_impl&&) = delete;
  chat_handler_impl& operator=(chat_handler_impl const&) = delete;
  chat_handler_impl& operator=(chat_handler_impl&&) = delete;
  ~chat_handler_impl() override = default;

  boost::asio::awaitable<result<void>> handle(chat_http_request& request,
                                              response_sink& sink) override;

private:
  account_pool& m_pool;
  qoder::catalog::catalog_service& m_catalog;
  qoder::auth::token_service& m_tokens;
  qoder::chat::chat_client& m_client;
  openai::selection::session_store& m_sessions;
  clock& m_time;
  /// The `"{site}/"` prefix of `model` is stripped against this id, not against the pool's.
  std::string m_site_id;
};

namespace detail {

struct opened_upstream {
  std::unique_ptr<qoder::chat::chat_stream> m_stream;
  /// The catalogue key, never the client's model string: that is what an OpenAI `model` reports.
  std::string m_model_key;
  std::string m_task_id;
};

/// Account and model selection plus failover of §5.2.
///
/// A returned stream is final. Its first event is already read, so the account can no longer be
/// swapped and every later failure belongs to the writer as an error frame.
///
/// `session_header` is the raw `X-Qoder2Api-Session` / `X-Session-Id` value. The affinity key of
/// §6.3 is the derived session id of §4.1, which also covers a request that carries no header.
[[nodiscard]] boost::asio::awaitable<result<opened_upstream>>
open_upstream(account_pool& pool, qoder::catalog::catalog_service& catalog,
              qoder::auth::token_service& tokens, qoder::chat::chat_client& client,
              openai::selection::session_store& sessions, clock& time, json body,
              std::string_view model, std::string_view session_header);

} // namespace detail

} // namespace qoderz::server::endpoints
