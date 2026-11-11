#include "common/net/detail/tls.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <mutex>
#include <string>

namespace qoderz::net::detail {
namespace {

std::once_flag& init_flag() {
  static std::once_flag flag;
  return flag;
}

std::unique_ptr<boost::asio::ssl::context>& stored_context() {
  static std::unique_ptr<boost::asio::ssl::context> context;
  return context;
}

} // namespace

boost::asio::ssl::context& tls_context::instance() {
  std::call_once(init_flag(), [] {
    auto context =
        std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
    context->set_default_verify_paths();
    context->set_verify_mode(boost::asio::ssl::verify_peer);
    stored_context() = std::move(context);
  });
  return *stored_context();
}

} // namespace qoderz::net::detail
