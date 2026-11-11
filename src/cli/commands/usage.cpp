#include "qoder/usage/usage.hpp"

#include "cli/commands/models.hpp"
#include "cli/commands/usage.hpp"
#include "cli/output.hpp"
#include "common/clock.hpp"
#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "qoder/auth/token.hpp"

#include <boost/asio/io_context.hpp>
#include <optional>
#include <string>
#include <utility>

namespace qoderz::cli {

int run_usage(command_context& context, query_options const& options) {
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
  qoder::usage::usage_service usage{http, time};

  auto fresh = run_blocking(io, tokens.ensure_fresh(target->m_account));
  if (!fresh.has_value()) {
    return report_error(context.m_app, fresh.error());
  }
  auto snapshot = run_blocking(io, usage.fetch(target->m_site, *fresh));
  if (!snapshot.has_value()) {
    return report_error(context.m_app, snapshot.error());
  }

  if (options.m_json) {
    json windows = json::array();
    for (auto const& window : snapshot->m_windows) {
      windows.push_back(json{{"name", window.m_name},
                             {"used_percent", window.m_percent},
                             {"display", window.m_display}});
    }
    json body;
    body["plan"] = snapshot->m_plan.has_value() ? json(*snapshot->m_plan) : json(nullptr);
    auto const until = format_iso8601(snapshot->m_until_ms);
    body["until"] = until.has_value() ? json(*until) : json(nullptr);
    body["windows"] = std::move(windows);
    write_json(context.m_app.m_out, body);
    return code_of(exit_code::success);
  }

  auto& out = context.m_app.m_out;
  out << "Plan: " << snapshot->m_plan.value_or("-") << '\n';
  out << "Expires: " << format_local_time(snapshot->m_until_ms) << '\n';
  if (snapshot->m_windows.empty()) {
    out << "No usage windows.\n";
  } else {
    for (auto const& window : snapshot->m_windows) {
      out << window.m_name << ": " << window.m_display << " (" << window.m_percent << "%)\n";
    }
  }
  out.flush();
  return code_of(exit_code::success);
}

} // namespace qoderz::cli
