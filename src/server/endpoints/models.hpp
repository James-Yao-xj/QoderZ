#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "server/pool/account_pool.hpp"

#include <boost/asio/awaitable.hpp>
#include <string>

namespace qoderz::server::endpoints {

/// `/v1/models`: the union of the model catalogues of every healthy account, per
/// docs/design/server.md §4.
///
/// The handler never rotates and never touches the session binding: listing models must not change
/// which account a chat session sticks to.
class models_handler {
public:
  models_handler(account_pool& pool, qoder::catalog::catalog_service& catalog,
                 qoder::auth::token_service& tokens, clock& time);

  models_handler(models_handler const&) = delete;
  models_handler(models_handler&&) = delete;
  models_handler& operator=(models_handler const&) = delete;
  models_handler& operator=(models_handler&&) = delete;
  ~models_handler() = default;

  /// The OpenAI model list as JSON, or the error to answer with.
  ///
  /// An empty union is not an empty list: with a recorded failure the last one wins, otherwise the
  /// pool explains why it had no candidate.
  [[nodiscard]] boost::asio::awaitable<result<std::string>> build_json();

private:
  account_pool& m_pool;
  qoder::catalog::catalog_service& m_catalog;
  qoder::auth::token_service& m_tokens;
  clock& m_time;
};

} // namespace qoderz::server::endpoints
