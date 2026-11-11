#include "cli/commands/serve/wiring.hpp"

#include "cli/output.hpp"
#include "common/json.hpp"
#include "common/log.hpp"
#include "common/strings.hpp"
#include "common/version.hpp"
#include "config/accounts.hpp"

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <istream>
#include <string>
#include <thread>
#include <utility>

namespace qoderz::cli {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view default_listen = "http://127.0.0.1:8788";
constexpr std::size_t default_body_limit = 32 * 1024 * 1024;

std::vector<std::string> split_selectors(std::string const& text) {
  std::vector<std::string> selectors;
  std::size_t start = 0;
  while (start <= text.size()) {
    auto const comma = text.find(',', start);
    auto const piece = strings::trim(std::string_view{text}.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!piece.empty()) {
      selectors.emplace_back(piece);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return selectors;
}

result<config::account_record const*>
one_account(config::credential_file const& file, std::string_view site, std::string_view selector) {
  std::string_view const sites[] = {site};
  auto const matches = match_accounts_across(file, sites, selector);
  if (matches.empty()) {
    return std::unexpected(
        configuration_error("no account '" + std::string{selector} + "' for " + std::string{site}));
  }
  if (matches.size() > 1) {
    std::string keys;
    for (auto const& match : matches) {
      if (!keys.empty()) {
        keys.append(", ");
      }
      keys.append(match.m_key);
    }
    return std::unexpected(
        configuration_error("'" + std::string{selector} + "' is ambiguous: " + keys));
  }
  return matches.front().m_account;
}

result<std::optional<std::string>> resolve_api_key(serve_options const& options,
                                                   env_options const& env, app_context& app) {
  auto const sources = static_cast<int>(!options.m_api_key.empty()) +
                       static_cast<int>(options.m_api_key_stdin) +
                       static_cast<int>(env.m_api_key.has_value());
  if (sources > 1) {
    return std::unexpected(
        bad_request_error("use only one of --api-key, --api-key-stdin and QODERZ_API_KEY"));
  }
  if (!options.m_api_key.empty()) {
    auto const trimmed = strings::trim(options.m_api_key);
    if (trimmed.empty()) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>{std::string{trimmed}};
  }
  if (options.m_api_key_stdin) {
    std::string line;
    if (!std::getline(app.m_in, line)) {
      return std::unexpected(
          configuration_error("--api-key-stdin could not read a line from standard input"));
    }
    auto const trimmed = strings::trim(line);
    if (trimmed.empty()) {
      return std::unexpected(configuration_error("--api-key-stdin read an empty line"));
    }
    return std::optional<std::string>{std::string{trimmed}};
  }
  if (env.m_api_key.has_value()) {
    return std::optional<std::string>{*env.m_api_key};
  }
  return std::optional<std::string>{};
}

} // namespace

result<serve_plan> plan_serve(command_context& context, serve_options const& options) {
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
  auto const available = config::accounts_for_site(*file, (*site)->m_id);
  if (available.empty()) {
    return std::unexpected(configuration_error("not signed in; run: qoderz login"));
  }

  auto const selectors = split_selectors(options.m_accounts);
  if (!options.m_account.empty() && !selectors.empty()) {
    return std::unexpected(bad_request_error("--account and --accounts are mutually exclusive"));
  }

  std::optional<std::string> selector;
  if (!options.m_account.empty()) {
    selector = options.m_account;
  }
  if (!selector.has_value() && selectors.empty()) {
    selector = context.m_env.m_account;
  }

  std::vector<config::account_record const*> chosen;
  if (selector.has_value()) {
    auto account = one_account(*file, (*site)->m_id, *selector);
    if (!account.has_value()) {
      return std::unexpected(std::move(account.error()));
    }
    chosen.push_back(*account);
  } else if (!selectors.empty()) {
    for (auto const& item : selectors) {
      auto account = one_account(*file, (*site)->m_id, item);
      if (!account.has_value()) {
        return std::unexpected(std::move(account.error()));
      }
      bool const duplicate =
          std::any_of(chosen.begin(), chosen.end(),
                      [&](config::account_record const* seen) { return seen == *account; });
      if (duplicate) {
        return std::unexpected(configuration_error("--accounts lists '" + item + "' twice"));
      }
      chosen.push_back(*account);
    }
  } else {
    chosen = available;
  }

  std::vector<server::pool_account> accounts;
  accounts.reserve(chosen.size());
  for (auto const* account : chosen) {
    auto resolved = resolve_site_with_vpc(**site, account->m_vpc, *upstream);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    accounts.push_back(server::pool_account{
        .m_key = account->key(), .m_account = *account, .m_site = std::move(*resolved)});
  }

  server::pool_options pool_options;
  if (selector.has_value()) {
    pool_options.m_pinned_key = accounts.front().m_key;
  } else if (!selectors.empty()) {
    for (auto const& account : accounts) {
      pool_options.m_only_keys.push_back(account.m_key);
    }
  }
  pool_options.m_ignore_cooldown = options.m_ignore_cooldown;

  auto const prepared = server::account_pool::prepare(accounts, pool_options);
  if (!prepared.has_value()) {
    return std::unexpected(std::move(prepared.error()));
  }

  auto const listen_text = !options.m_listen.empty()
                               ? options.m_listen
                               : context.m_env.m_listen.value_or(std::string{default_listen});
  auto listen = parse_listen(listen_text);
  if (!listen.has_value()) {
    return std::unexpected(std::move(listen.error()));
  }

  auto key = resolve_api_key(options, context.m_env, context.m_app);
  if (!key.has_value()) {
    return std::unexpected(std::move(key.error()));
  }

  auto const threads = !options.m_threads.empty() ? parse_threads(options.m_threads)
                                                  : resolve_threads(context.m_env);
  if (!threads.has_value()) {
    return std::unexpected(std::move(threads.error()));
  }

  auto const body_limit = !options.m_max_body_bytes.empty()
                              ? parse_body_limit(options.m_max_body_bytes)
                              : result<std::size_t>{default_body_limit};
  if (!body_limit.has_value()) {
    return std::unexpected(std::move(body_limit.error()));
  }

  server::server_config config;
  config.m_listen_host = listen->m_host;
  config.m_port = listen->m_port;
  config.m_site_id = std::string{(*site)->m_id};
  config.m_api_key = *key;
  config.m_max_body_bytes = *body_limit;
  config.m_allow_public = options.m_allow_public;

  if (auto const valid = server::validate_config(config); !valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }

  return serve_plan{.m_config = std::move(config),
                    .m_accounts = std::move(accounts),
                    .m_pool_options = std::move(pool_options),
                    .m_site = **site,
                    .m_credentials_path = std::move(*path),
                    .m_upstream = *upstream,
                    .m_threads = *threads,
                    .m_json = options.m_json};
}

serve_runtime::serve_runtime(serve_plan plan,
                             std::unique_ptr<protocol::pem_rsa_key_wrapper> wrapper)
    : m_http{m_io}, m_catalog{m_http, m_time}, m_store{plan.m_credentials_path},
      m_wrapper{std::move(wrapper)},
      m_tokens{m_http, m_store,
               [site = plan.m_site,
                upstream = plan.m_upstream](std::string_view vpc) -> result<config::resolved_site> {
                 return resolve_site_with_vpc(site, vpc, upstream);
               },
               m_time},
      m_chat{m_http, *m_wrapper, m_time}, m_sessions{}, m_health{m_time}, m_affinity{},
      m_listen_host{plan.m_config.m_listen_host}, m_site_id{plan.m_config.m_site_id},
      m_max_body_bytes{plan.m_config.m_max_body_bytes}, m_threads{plan.m_threads},
      m_json{plan.m_json}, m_upstream_loopback{plan.m_upstream.has_value()},
      m_allow_public{plan.m_config.m_allow_public},
      m_mode{plan.m_pool_options.m_pinned_key.has_value()
                 ? "pinned"
                 : (plan.m_pool_options.m_only_keys.empty() ? "pool" : "subset")} {
  m_pool = std::make_unique<server::account_pool>(std::move(plan.m_accounts), m_health, m_affinity,
                                                  std::move(plan.m_pool_options),
                                                  plan.m_config.m_site_id);
  m_models.emplace(*m_pool, m_catalog, m_tokens, m_time);
  m_chat_handler.emplace(*m_pool, m_catalog, m_tokens, m_chat, m_sessions, m_time,
                         plan.m_config.m_site_id);
  m_server.emplace(m_io, std::move(plan.m_config),
                   server::server_dependencies{.m_chat = *m_chat_handler, .m_models = *m_models});
}

result<std::unique_ptr<serve_runtime>> serve_runtime::create(serve_plan plan) {
  auto wrapper = protocol::pem_rsa_key_wrapper::create();
  if (!wrapper.has_value()) {
    return std::unexpected(std::move(wrapper.error()));
  }
  return std::make_unique<serve_runtime>(std::move(plan), std::move(*wrapper));
}

serve_runtime::~serve_runtime() = default;

result<std::uint16_t> serve_runtime::start() {
  auto started = m_server->start();
  if (!started.has_value()) {
    return std::unexpected(std::move(started.error()));
  }
  m_port = *started;
  boost::asio::co_spawn(m_io, m_server->run(), boost::asio::detached);
  return m_port;
}

void serve_runtime::print_summary(app_context& context, std::uint16_t port) const {
  std::string const listen = "http://" + m_listen_host + ":" + std::to_string(port);
  if (m_json) {
    write_json(context.m_out, json{{"version", std::string{version::string}},
                                   {"listen", listen},
                                   {"site", m_site_id},
                                   {"accounts", m_pool->size()},
                                   {"mode", m_mode},
                                   {"threads", m_threads},
                                   {"max_body_bytes", m_max_body_bytes},
                                   {"upstream_loopback", m_upstream_loopback}});
  }
  context.m_err << "qoderz " << version::string << '\n';
  context.m_err << "  listen:   " << listen << '\n';
  context.m_err << "  site:     " << m_site_id << '\n';
  context.m_err << "  accounts: " << m_pool->size() << " (" << m_mode << ")\n";
  context.m_err << "  threads:  " << m_threads << '\n';
  context.m_err << "  body:     " << m_max_body_bytes << " bytes\n";
  context.m_err << "  upstream: " << (m_upstream_loopback ? "loopback stub" : "site hosts") << '\n';
  if (m_allow_public) {
    context.m_err << "  warning:  public listener (--allow-public)\n";
  }
  context.m_err.flush();
}

boost::asio::awaitable<void> serve_runtime::drain() {
  co_await m_server->drain();
}

boost::asio::io_context& serve_runtime::io() noexcept {
  return m_io;
}

server::server& serve_runtime::listener() noexcept {
  return *m_server;
}

std::uint16_t serve_runtime::port() const noexcept {
  return m_port;
}

std::string const& serve_runtime::mode() const noexcept {
  return m_mode;
}

std::size_t serve_runtime::account_count() const noexcept {
  return m_pool->size();
}

int serve_runtime::serve(app_context& context) {
  auto started = start();
  if (!started.has_value()) {
    return report_error(context, started.error());
  }

  print_summary(context, *started);

  auto guard = boost::asio::make_work_guard(m_io);
  auto signals = std::make_shared<boost::asio::signal_set>(m_io, SIGINT, SIGTERM);
  auto seen = std::make_shared<int>(0);
  auto rearm = std::make_shared<std::function<void()>>();
  *rearm = [this, signals, seen, rearm]() {
    signals->async_wait(
        [this, signals, seen, rearm](boost::system::error_code const& ec, int number) {
          if (ec) {
            return;
          }
          if (++(*seen) > 1) {
            std::_Exit(code_of(exit_code::interrupted));
          }
          log(log_level::info, "shutting down", {{"signal", std::to_string(number)}});
          m_server->stop();
          boost::asio::co_spawn(
              m_io,
              [this]() -> boost::asio::awaitable<void> {
                co_await m_server->drain();
                m_io.stop();
              },
              boost::asio::detached);
          (*rearm)();
        });
  };
  (*rearm)();

  std::vector<std::jthread> workers;
  workers.reserve(m_threads > 0 ? m_threads - 1 : 0);
  for (unsigned index = 1; index < m_threads; ++index) {
    workers.emplace_back([this] { m_io.run(); });
  }
  m_io.run();
  for (auto& worker : workers) {
    worker.join();
  }
  return code_of(exit_code::success);
}

} // namespace qoderz::cli
