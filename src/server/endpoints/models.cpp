#include "server/endpoints/models.hpp"

#include "common/json.hpp"
#include "common/log.hpp"
#include "common/strings.hpp"
#include "qoder/catalog/model.hpp"

#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qoderz::server::endpoints {
namespace {

namespace catalog = qoder::catalog;

/// The session key `/v1/models` uses for candidate selection. It is a fixed name, so the call can
/// never collide with a real chat session id and can never move a binding.
constexpr std::string_view models_session = "models";

constexpr std::string_view owner = "qoder";

bool cools_down(error_kind kind) noexcept {
  return kind == error_kind::auth || kind == error_kind::quota;
}

failure_kind cooling_of(error_kind kind) noexcept {
  return kind == error_kind::auth ? failure_kind::auth : failure_kind::quota;
}

json model_json(catalog::model_info const& model, std::int64_t created) {
  return json{{"id", model.m_key},
              {"object", "model"},
              {"created", created},
              {"owned_by", owner},
              {"context_window", model.m_context},
              {"supports_images", model.m_images},
              {"supports_tools", true},
              {"reasoning", !model.m_efforts.empty() || model.m_thinks}};
}

} // namespace

models_handler::models_handler(account_pool& pool, catalog::catalog_service& catalog_service,
                               qoder::auth::token_service& tokens, clock& time)
    : m_pool{pool}, m_catalog{catalog_service}, m_tokens{tokens}, m_time{time} {}

boost::asio::awaitable<result<std::string>> models_handler::build_json() {
  std::vector<catalog::model_info> merged;
  std::unordered_set<std::string> seen;
  std::optional<error> last_failure;

  for (auto const* candidate : m_pool.candidates(models_session, false)) {
    auto fresh = co_await m_tokens.ensure_fresh(candidate->m_account);
    if (!fresh) {
      auto failure = std::move(fresh.error());
      if (cools_down(failure.m_kind)) {
        m_pool.report_failure(candidate->m_key, cooling_of(failure.m_kind));
      } else {
        log(log_level::warn, "skipping an account while listing models",
            {{"account", candidate->m_key}, {"stage", "token"}});
      }
      last_failure = std::move(failure);
      continue;
    }

    auto models = co_await m_catalog.fetch(candidate->m_site, *fresh, false);
    if (!models) {
      auto failure = std::move(models.error());
      if (cools_down(failure.m_kind)) {
        m_pool.report_failure(candidate->m_key, cooling_of(failure.m_kind));
      } else {
        log(log_level::warn, "skipping an account while listing models",
            {{"account", candidate->m_key}, {"stage", "catalog"}});
      }
      last_failure = std::move(failure);
      continue;
    }

    m_pool.report_success(candidate->m_key);
    for (auto& model : *models) {
      if (seen.emplace(strings::ascii_lower_copy(model.m_key)).second) {
        merged.push_back(std::move(model));
      }
    }
  }

  if (merged.empty()) {
    if (last_failure.has_value()) {
      co_return std::unexpected(std::move(*last_failure));
    }
    co_return std::unexpected(m_pool.no_candidates_error());
  }

  // One `created` for the whole list: it is the moment this response was generated, not the
  // moment each model was first seen.
  auto const created = static_cast<std::int64_t>(m_time.now_ms() / 1000);
  json data = json::array();
  for (auto const& model : merged) {
    data.push_back(model_json(model, created));
  }
  co_return json{{"object", "list"}, {"data", std::move(data)}}.dump();
}

} // namespace qoderz::server::endpoints
