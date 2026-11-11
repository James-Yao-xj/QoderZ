#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

namespace qoderz::net::detail {

/// The shared `ssl::context` for every TLS connection this process opens.
///
/// TLS 1.2+ client mode, the system trust store, `verify_peer` and host-name verification are
/// always on; there is deliberately no switch to relax any of them, because a gateway that can be
/// told to skip verification leaks upstream credentials the moment an operator is in a hurry.
class tls_context {
public:
  /// Returns the process-wide context, creating it on first use. OpenSSL setup failures are
  /// unrecoverable, so they surface as an exception here and are converted by the caller.
  [[nodiscard]] static boost::asio::ssl::context& instance();

  tls_context() = delete;
};

} // namespace qoderz::net::detail
