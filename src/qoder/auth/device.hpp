#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"
#include "qoder/auth/support/browser.hpp"
#include "qoder/auth/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <functional>
#include <string_view>

namespace qoderz::qoder::auth {

/// Hands the device URL to the user; `open_browser` by default, a no-op in tests.
using browser_launcher = std::function<result<void>(std::string_view)>;

/// The PKCE-shaped parameters of one device sign-in, kept together because the poll, the exchange
/// and the userinfo call all have to agree on them.
struct device_challenge {
  std::string m_verifier;
  std::string m_challenge;
  std::string m_nonce;
  std::string m_machine_id;
  std::string m_url;
};

/// The device authorization flow of docs/protocol/qoder-auth.md §2.
///
/// One `login` call runs the whole sequence: build the challenge, report the URL, poll until the
/// token pair arrives, exchange it for a job token (falling back to the device pair on a CN site
/// that answers 4xx), and read the userinfo. The references it holds must outlive it.
class device_flow {
public:
  device_flow(net::http_client& http, config::site_definition const& site,
              config::resolved_site const& resolved, clock& time,
              browser_launcher launcher = open_browser);

  /// The timeout is a `steady_clock` budget, so a frozen wall clock cannot shorten or stretch it.
  /// Progress fires once before the first poll and once per poll, so the CLI can show that the wait
  /// is alive without pretending to know when the user will authorize.
  [[nodiscard]] boost::asio::awaitable<result<config::account_record>>
  login(login_options const& options, progress_callback const& progress = {});

  /// Verifier, challenge, nonce, machine id and the URL, without touching the network.
  [[nodiscard]] device_challenge make_challenge() const;

private:
  net::http_client& m_http;
  config::site_definition const& m_site;
  config::resolved_site const& m_resolved;
  clock& m_time;
  browser_launcher m_launcher;
};

} // namespace qoderz::qoder::auth
