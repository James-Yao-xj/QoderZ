#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace qoderz::test {

/// Runs one awaitable to completion on `io` and returns what it produced.
///
/// The test thread pumps the same `io_context` the stub server runs on, so client and server make
/// progress together and nothing has to sleep to fake synchronisation. A `timeout` guards the whole
/// run: if the awaitable is still pending the context is stopped and `std::nullopt` comes back,
/// which a test asserts on instead of hanging CTest. An exception escaping the awaitable is
/// rethrown here, so Catch2 reports it the way it would report a synchronous failure.
template <class value_type>
std::optional<value_type>
run_awaitable(boost::asio::io_context& io, boost::asio::awaitable<value_type> awaitable,
              std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
  io.restart();
  std::optional<value_type> outcome;
  std::exception_ptr failure;
  bool timed_out = false;

  boost::asio::steady_timer guard{io, timeout};
  guard.async_wait([&](std::error_code const& ec) {
    if (ec) {
      return;
    }
    timed_out = true;
    io.stop();
  });

  boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
        outcome.emplace(co_await std::move(awaitable));
        co_return;
      },
      [&](std::exception_ptr escaped) {
        failure = std::move(escaped);
        guard.cancel();
        io.stop();
      });

  io.run();
  guard.cancel();

  if (failure) {
    std::rethrow_exception(failure);
  }
  if (timed_out || !outcome.has_value()) {
    return std::nullopt;
  }
  return std::move(outcome);
}

} // namespace qoderz::test
