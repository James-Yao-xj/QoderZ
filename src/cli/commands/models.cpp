#include "cli/commands/models.hpp"

#include "cli/output.hpp"
#include "common/clock.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/model.hpp"
#include "qoder/catalog/service.hpp"

#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::cli {

result<query_target> prepare_query(command_context& context, query_options const& options) {
  auto site = resolve_site_definition(context.m_cli, context.m_env);
  if (!site.has_value()) {
    return std::unexpected(std::move(site.error()));
  }
  auto upstream = upstream_origin(context.m_env);
  if (!upstream.has_value()) {
    return std::unexpected(std::move(upstream.error()));
  }
  auto path = resolve_credentials_path(context.m_cli, context.m_env);
  if (!path.has_value()) {
    return std::unexpected(std::move(path.error()));
  }

  config::credential_store store{*path};
  auto file = store.load();
  if (!file.has_value()) {
    return std::unexpected(std::move(file.error()));
  }

  auto const selector = !options.m_account.empty() ? std::optional<std::string>{options.m_account}
                                                   : context.m_env.m_account;
  auto account = select_account(*file, (*site)->m_id, selector, context);
  if (!account.has_value()) {
    return std::unexpected(std::move(account.error()));
  }
  auto resolved = resolve_site_with_vpc(**site, (*account)->m_vpc, *upstream);
  if (!resolved.has_value()) {
    return std::unexpected(std::move(resolved.error()));
  }

  return query_target{.m_account = **account,
                      .m_site = std::move(*resolved),
                      .m_store_path = std::move(*path),
                      .m_site_definition = **site,
                      .m_upstream = *upstream};
}

int run_models(command_context& context, query_options const& options) {
  auto target = prepare_query(context, options);
  if (!target.has_value()) {
    return report_error(context.m_app, target.error());
  }

  config::credential_store store{target->m_store_path};
  boost::asio::io_context io;
  net::http_client http{io};
  qoderz::system_clock time;
  auto const site = target->m_site_definition;
  auto const upstream = target->m_upstream;
  qoder::auth::token_service tokens{
      http, store,
      [site, upstream](std::string_view vpc) { return resolve_site_with_vpc(site, vpc, upstream); },
      time};
  qoder::catalog::catalog_service catalog{http, time};

  auto fresh = run_blocking(io, tokens.ensure_fresh(target->m_account));
  if (!fresh.has_value()) {
    return report_error(context.m_app, fresh.error());
  }
  auto models = run_blocking(io, catalog.fetch(target->m_site, *fresh, true));
  if (!models.has_value()) {
    return report_error(context.m_app, models.error());
  }

  if (options.m_json) {
    json data = json::array();
    for (auto const& model : *models) {
      data.push_back(json{{"id", model.m_key},
                          {"object", "model"},
                          {"owned_by", "qoder"},
                          {"context_window", model.m_context},
                          {"supports_images", model.m_images},
                          {"supports_tools", true},
                          {"reasoning", model.m_efforts}});
    }
    write_json(context.m_app.m_out, json{{"object", "list"}, {"data", std::move(data)}});
    return code_of(exit_code::success);
  }

  if (models->empty()) {
    context.m_app.m_err << "no models are available for " << target->m_site.m_site_id << '\n';
    return code_of(exit_code::success);
  }

  table view;
  view.m_columns = {{.m_header = "ID"},
                    {.m_header = "NAME"},
                    {.m_header = "CONTEXT", .m_right = true},
                    {.m_header = "IMAGES"},
                    {.m_header = "REASONING"}};
  for (auto const& model : *models) {
    view.m_rows.push_back({model.m_key, model.m_name, format_context(model.m_context),
                           yes_no(model.m_images),
                           yes_no(!model.m_efforts.empty() || model.m_thinks)});
  }
  context.m_app.m_out << render(view);
  context.m_app.m_out.flush();
  return code_of(exit_code::success);
}

} // namespace qoderz::cli
