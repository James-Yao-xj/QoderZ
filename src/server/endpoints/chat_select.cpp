#include "common/log.hpp"
#include "common/strings.hpp"
#include "openai/request/body.hpp"
#include "openai/request/request.hpp"
#include "qoder/errors.hpp"
#include "server/endpoints/chat.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::server::endpoints::detail {
namespace {

namespace catalog = qoder::catalog;

/// Two accounts may describe one key differently, and the Qoder body has to carry the version of
/// the account that actually serves the request, so the catalogue entry travels with its account.
struct ready_candidate {
  pool_account const* m_account = nullptr;
  catalog::model_info m_model;
  config::account_record m_record;
};

struct unseen_candidate {
  pool_account const* m_account = nullptr;
  config::account_record m_record;
};

catalog::model_info const* find_model(std::vector<catalog::model_info> const& models,
                                      std::string_view key) {
  for (auto const& model : models) {
    if (strings::equals_ignore_case(model.m_key, key)) {
      return &model;
    }
  }
  return nullptr;
}

/// An error body is JSON, so a message cut inside a UTF-8 sequence would make it unparsable.
std::string truncate_message(std::string text, std::size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  text.resize(cut);
  return text;
}

std::string unknown_model_message(std::string_view model) {
  std::string message = "unknown or disabled model \"";
  message.append(model);
  message.push_back('"');
  return truncate_message(std::move(message), qoder::max_error_message_length);
}

/// Only auth and quota take an account out of rotation. A busy queue or a broken upstream must not
/// move the request to another account, so those return false and the caller answers with the
/// failure itself.
bool report_if_cooling(account_pool& pool, pool_account const& candidate, std::string_view stage,
                       error const& failure) {
  if (failure.m_kind != error_kind::auth && failure.m_kind != error_kind::quota) {
    return false;
  }
  auto const kind = failure.m_kind == error_kind::auth ? failure_kind::auth : failure_kind::quota;
  pool.report_failure(candidate.m_key, kind);
  log(log_level::warn, "chat account cooled down",
      {{"account", candidate.m_key}, {"stage", stage}, {"kind", to_string(failure.m_kind)}});
  return true;
}

/// The second pass exists because a listing can be missing from a cache that was filled before the
/// model was enabled. It only revisits accounts that did not fail, so a cooling account is never
/// asked twice.
boost::asio::awaitable<void> collect_ready(account_pool& pool, catalog::catalog_service& catalog,
                                           qoder::auth::token_service& tokens,
                                           std::vector<pool_account const*> const& candidates,
                                           std::string_view model,
                                           std::vector<ready_candidate>& ready,
                                           std::optional<error>& last_failure) {
  std::vector<unseen_candidate> unseen;

  for (auto const* candidate : candidates) {
    auto fresh = co_await tokens.ensure_fresh(candidate->m_account);
    if (!fresh) {
      auto failure = std::move(fresh.error());
      if (!report_if_cooling(pool, *candidate, "token", failure)) {
        last_failure = std::move(failure);
        co_return;
      }
      last_failure = std::move(failure);
      continue;
    }

    auto models = co_await catalog.fetch(candidate->m_site, *fresh, false);
    if (!models) {
      auto failure = std::move(models.error());
      if (!report_if_cooling(pool, *candidate, "catalog", failure)) {
        last_failure = std::move(failure);
        co_return;
      }
      last_failure = std::move(failure);
      continue;
    }

    pool.report_success(candidate->m_key);
    if (auto const* found = find_model(*models, model); found != nullptr) {
      ready.push_back(
          ready_candidate{.m_account = candidate, .m_model = *found, .m_record = *fresh});
      continue;
    }
    unseen.push_back(unseen_candidate{.m_account = candidate, .m_record = std::move(*fresh)});
  }

  if (!ready.empty() || unseen.empty()) {
    co_return;
  }

  for (auto& entry : unseen) {
    auto models = co_await catalog.fetch(entry.m_account->m_site, entry.m_record, true);
    if (!models) {
      auto failure = std::move(models.error());
      if (!report_if_cooling(pool, *entry.m_account, "catalog", failure)) {
        last_failure = std::move(failure);
        co_return;
      }
      last_failure = std::move(failure);
      continue;
    }

    pool.report_success(entry.m_account->m_key);
    if (auto const* found = find_model(*models, model); found != nullptr) {
      ready.push_back(ready_candidate{
          .m_account = entry.m_account, .m_model = *found, .m_record = std::move(entry.m_record)});
    }
  }
}

} // namespace

boost::asio::awaitable<result<opened_upstream>>
open_upstream(account_pool& pool, qoder::catalog::catalog_service& catalog,
              qoder::auth::token_service& tokens, qoder::chat::chat_client& client,
              openai::selection::session_store& sessions, clock& time, json const& body,
              std::string_view model, std::string_view session_id) {
  auto const candidates = pool.candidates(session_id, true);
  if (candidates.empty()) {
    co_return std::unexpected(pool.no_candidates_error());
  }

  std::vector<ready_candidate> ready;
  std::optional<error> last_failure;
  co_await collect_ready(pool, catalog, tokens, candidates, model, ready, last_failure);

  if (ready.empty()) {
    if (last_failure.has_value()) {
      co_return std::unexpected(std::move(*last_failure));
    }
    co_return std::unexpected(bad_request_error(unknown_model_message(model)));
  }

  std::optional<std::string_view> header;
  if (!session_id.empty()) {
    header = session_id;
  }

  for (auto const& entry : ready) {
    auto context = openai::prepare_request(body, header, entry.m_model, sessions, time);
    if (!context) {
      co_return std::unexpected(std::move(context.error()));
    }
    auto built = openai::build_qoder_body(*context);
    if (!built) {
      co_return std::unexpected(std::move(built.error()));
    }

    qoder::chat::upstream_request upstream;
    upstream.m_site = entry.m_account->m_site;
    upstream.m_account = entry.m_record;
    upstream.m_body_json = std::move(built->m_json);
    upstream.m_model_key = entry.m_model.m_key;
    upstream.m_model_source = entry.m_model.m_source;

    auto stream = co_await client.open(upstream);
    if (!stream) {
      auto failure = std::move(stream.error());
      if (!report_if_cooling(pool, *entry.m_account, "open", failure)) {
        co_return std::unexpected(std::move(failure));
      }
      last_failure = std::move(failure);
      continue;
    }

    pool.report_success(entry.m_account->m_key);
    pool.bind(session_id, entry.m_account->m_key);

    opened_upstream opened;
    opened.m_stream = std::move(*stream);
    opened.m_model_key = entry.m_model.m_key;
    opened.m_task_id = context->m_task.m_id;
    co_return opened;
  }

  if (last_failure.has_value()) {
    co_return std::unexpected(std::move(*last_failure));
  }
  co_return std::unexpected(internal_error("the chat request had no usable account"));
}

} // namespace qoderz::server::endpoints::detail
