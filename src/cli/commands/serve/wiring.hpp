#pragma once

#include "cli/app.hpp"
#include "cli/options.hpp"
#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "openai/selection/session.hpp"
#include "protocol/cosy_signer.hpp"
#include "qoder/auth/token.hpp"
#include "qoder/catalog/service.hpp"
#include "qoder/chat/client.hpp"
#include "server/config.hpp"
#include "server/endpoints/chat.hpp"
#include "server/endpoints/models.hpp"
#include "server/pool/account_pool.hpp"
#include "server/pool/affinity.hpp"
#include "server/pool/health.hpp"
#include "server/server.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qoderz::cli {

/// `qoderz serve` of `docs/design/cli.md` §7, as the command line and the environment expressed it.
struct serve_options {
  std::string m_listen;
  std::string m_account;
  std::string m_accounts;
  std::string m_api_key;
  bool m_api_key_stdin = false;
  std::string m_threads;
  std::string m_max_body_bytes;
  bool m_ignore_cooldown = false;
  bool m_allow_public = false;
  bool m_json = false;
};

/// A validated startup: the listener configuration, the account snapshot and how the pool was
/// narrowed. Nothing here touches the network or binds a socket.
struct serve_plan {
  server::server_config m_config;
  std::vector<server::pool_account> m_accounts;
  server::pool_options m_pool_options;
  config::site_definition m_site;
  std::filesystem::path m_credentials_path;
  std::optional<std::string> m_upstream;
  unsigned m_threads = 1;
  bool m_json = false;
};

/// Reads the credential store, applies the pool rules of `docs/design/server.md` §6.1 and the
/// safety check of §1. An empty pool, a missing pinned account and a public listener without a key
/// are all configuration errors, so `serve` exits 1 before binding anything.
[[nodiscard]] result<serve_plan> plan_serve(command_context& context, serve_options const& options);

/// Every object `serve` wires together, in construction order. A `serve` process keeps the account
/// snapshot it read at startup, so the pool and its handlers stay valid for the process lifetime.
struct serve_runtime {
  boost::asio::io_context m_io;
  qoderz::system_clock m_time;
  net::http_client m_http;
  qoder::catalog::catalog_service m_catalog;
  config::credential_store m_store;
  std::unique_ptr<protocol::pem_rsa_key_wrapper> m_wrapper;
  qoder::auth::token_service m_tokens;
  qoder::chat::chat_client m_chat;
  openai::selection::session_store m_sessions;
  server::account_health m_health;
  server::session_affinity m_affinity;
  std::unique_ptr<server::account_pool> m_pool;
  std::optional<server::endpoints::models_handler> m_models;
  std::optional<server::endpoints::chat_handler_impl> m_chat_handler;
  std::optional<server::server> m_server;

  std::uint16_t m_port = 0;
  std::string m_listen_host;
  std::string m_site_id;
  std::size_t m_max_body_bytes = 0;
  unsigned m_threads = 1;
  bool m_json = false;
  bool m_upstream_loopback = false;
  bool m_allow_public = false;
  std::string m_mode;

  /// Assembles the object graph. `create` is the entry point: it loads the signing key and
  /// validates the pool before anything is constructed, so a failure leaves nothing half-built.
  serve_runtime(serve_plan plan, std::unique_ptr<protocol::pem_rsa_key_wrapper> wrapper);

  [[nodiscard]] static result<std::unique_ptr<serve_runtime>> create(serve_plan plan);

  serve_runtime(serve_runtime const&) = delete;
  serve_runtime(serve_runtime&&) = delete;
  serve_runtime& operator=(serve_runtime const&) = delete;
  serve_runtime& operator=(serve_runtime&&) = delete;
  ~serve_runtime();

  /// Binds the listener and starts the accept loop on the io_context.
  [[nodiscard]] result<std::uint16_t> start();

  /// Prints the startup summary: version, listen, site, account count, mode, threads, body limit
  /// and whether the upstream is a loopback stub. Text goes to stderr, `--json` to stdout.
  void print_summary(app_context& context, std::uint16_t port) const;

  /// Runs until SIGINT/SIGTERM: stops accepting, drains for the shutdown grace and returns 0.
  [[nodiscard]] int serve(app_context& context);

  /// Stops the acceptor and drains without waiting for a signal; used by tests.
  [[nodiscard]] boost::asio::awaitable<void> drain();

  [[nodiscard]] boost::asio::io_context& io() noexcept;

  [[nodiscard]] server::server& listener() noexcept;

  [[nodiscard]] std::uint16_t port() const noexcept;

  [[nodiscard]] std::string const& mode() const noexcept;

  [[nodiscard]] std::size_t account_count() const noexcept;
};

} // namespace qoderz::cli
