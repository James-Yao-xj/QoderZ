#include "common/error.hpp"
#include "common/net/url.hpp"
#include "stub_upstream/server.hpp"
#include "stub_upstream/stub_logic.hpp"

#include <CLI/CLI.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace test = qoderz::test;

constexpr std::string_view default_listen = "http://127.0.0.1:18899";

int usage_error(std::string const& message) {
  std::cerr << "stub-upstream: " << message << '\n';
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  std::string listen{default_listen};
  int port = -1;
  std::size_t chunks = test::stub_upstream_options{}.m_chunks;
  std::size_t chunk_bytes = test::stub_upstream_options{}.m_chunk_bytes;

  CLI::App app{"Loopback stub of the Qoder upstream: no real host, no real token."};
  app.add_option("--listen", listen, "Origin to listen on: http://127.0.0.1:<port>");
  app.add_option("--port", port, "Override the listen port; 0 asks the kernel for a free one");
  app.add_option("--chunks", chunks, "Content frames in one chat generation");
  app.add_option("--chunk-bytes", chunk_bytes, "Bytes of 'd' in one content frame");
  try {
    app.parse(argc, argv);
  } catch (CLI::ParseError const& failure) {
    return usage_error(failure.what());
  }

  auto parsed = qoderz::net::parse_url(listen);
  if (!parsed) {
    return usage_error(parsed.error().m_message);
  }
  if (parsed->m_tls || !qoderz::net::is_loopback_host(parsed->m_host)) {
    return usage_error("--listen must be a plain http origin on 127.0.0.1");
  }

  std::uint16_t listen_port = 0;
  try {
    auto const value = std::stoi(parsed->m_port);
    if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
      return usage_error("--listen carries an out-of-range port");
    }
    listen_port = static_cast<std::uint16_t>(value);
  } catch (std::exception const&) {
    return usage_error("--listen carries a port that is not a number");
  }
  if (port >= 0) {
    if (port > std::numeric_limits<std::uint16_t>::max()) {
      return usage_error("--port is out of range");
    }
    listen_port = static_cast<std::uint16_t>(port);
  }
  if (chunks == 0 || chunk_bytes == 0) {
    return usage_error("--chunks and --chunk-bytes must be positive");
  }

  test::stub_upstream_options const options{.m_chunks = chunks, .m_chunk_bytes = chunk_bytes};

  boost::asio::io_context io;
  test::stub_upstream_server server{
      io, listen_port,
      [options](test::stub_request const& request) -> boost::asio::awaitable<test::stub_response> {
        auto routed = test::stub_route(request, options);
        if (!routed.m_handled) {
          std::fprintf(stderr, "unhandled path: %s %.*s\n", request.m_method.c_str(),
                       static_cast<int>(test::stub_path_of(request.m_target).size()),
                       test::stub_path_of(request.m_target).data());
          std::fflush(stderr);
        }
        co_return std::move(routed.m_response);
      }};

  auto started = server.start();
  if (!started) {
    std::cerr << "stub-upstream: " << started.error().m_message << '\n';
    return 1;
  }

  std::cout << "stub-upstream listening on " << server.origin() << '\n' << std::flush;

  boost::asio::signal_set signals{io, SIGINT, SIGTERM};
  signals.async_wait([&server, &io](boost::system::error_code const& failure, int) {
    if (failure) {
      return;
    }
    server.stop();
    io.stop();
  });

  io.run();
  return 0;
}
