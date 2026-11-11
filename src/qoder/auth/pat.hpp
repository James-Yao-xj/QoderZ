#pragma once

#include "common/clock.hpp"
#include "common/error.hpp"
#include "common/net/http_client.hpp"
#include "config/credentials.hpp"
#include "config/site.hpp"

#include <boost/asio/awaitable.hpp>
#include <string>

namespace qoderz::qoder::auth {

/// Checks a personal access token without any network access.
///
/// The stored value is the PAT verbatim, which is why the check lives on its own: a login command
/// must be able to reject a typo before it sends the secret anywhere, and the refresh path reuses
/// the same rule when it falls back to the PAT.
[[nodiscard]] result<std::string> validate_pat(std::string_view pat);

/// Exchanges a PAT for an access token and reads the account behind it (protocol §3).
///
/// The PAT itself never leaves the request body — there is no `Authorization` header on the
/// exchange — and it is never logged. A 400/401/403 is an auth error; any other non-200 is
/// upstream. The resulting record keeps the PAT so a later refresh can re-exchange it.
[[nodiscard]] boost::asio::awaitable<result<config::account_record>>
login_with_pat(net::http_client& http, config::site_definition const& site,
               config::resolved_site const& resolved, std::string pat, clock& time);

} // namespace qoderz::qoder::auth
