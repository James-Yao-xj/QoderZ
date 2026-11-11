#include "common/net/http_client.hpp"

#include "common/net/detail/connection.hpp"
#include "common/net/detail/connection_pool.hpp"
#include "common/net/url.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qoderz::net {
namespace {

namespace asio = boost::asio;
using detail::connection;
using detail::connection_pool;

using steady_clock = std::chrono::steady_clock;

std::unexpected<error> stage_error(std::string_view host, std::string_view stage,
                                   std::string_view detail) {
  return std::unexpected(upstream_error("The HTTP request to " + std::string{host} + " failed at " +
                                        std::string{stage} + ": " + std::string{detail}));
}

/// A request-wide budget. Each wait is given the time that is left rather than a fresh timeout, so
/// a peer that trickles bytes cannot stretch a request past its deadline.
class budget {
public:
  explicit budget(std::chrono::milliseconds total) : m_deadline{steady_clock::now() + total} {}

  [[nodiscard]] std::chrono::milliseconds remaining() const noexcept {
    auto const left = m_deadline - steady_clock::now();
    if (left <= steady_clock::duration::zero()) {
      return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
  }

private:
  steady_clock::time_point m_deadline;
};

/// A stream that owns its connection for the life of one response body.
///
/// The connection leaves it in exactly two ways: a fully read keep-alive message returns it to the
/// pool, and everything else — an error, a `Connection: close` response, or destruction before the
/// body ends — closes it. That is what keeps a half-read socket from being reused.
class connection_stream final : public http_stream {
public:
  connection_stream(std::shared_ptr<connection> connection, http_response header,
                    std::shared_ptr<connection_pool> pool, std::string origin,
                    std::chrono::milliseconds idle_timeout)
      : m_connection{std::move(connection)}, m_header{std::move(header)}, m_pool{std::move(pool)},
        m_origin{std::move(origin)}, m_idle_timeout{idle_timeout} {}

  connection_stream(connection_stream const&) = delete;
  connection_stream& operator=(connection_stream const&) = delete;

  ~connection_stream() override { release(); }

  [[nodiscard]] int status() const noexcept override { return m_header.m_status; }

  [[nodiscard]] std::vector<http_header> const& headers() const noexcept override {
    return m_header.m_headers;
  }

  boost::asio::awaitable<result<std::optional<std::string>>> read_some() override {
    if (!m_connection) {
      co_return std::optional<std::string>{};
    }
    auto chunk = co_await m_connection->read_some(m_idle_timeout);
    if (!chunk) {
      release();
      co_return std::unexpected(chunk.error());
    }
    if (!chunk->has_value()) {
      m_finished = true;
      release();
      co_return std::optional<std::string>{};
    }
    co_return chunk;
  }

private:
  void release() noexcept {
    auto connection = std::move(m_connection);
    if (!connection) {
      return;
    }
    if (m_finished && connection->keep_alive()) {
      m_pool->give(m_origin, std::move(connection));
      return;
    }
    connection->close();
  }

  std::shared_ptr<connection> m_connection;
  http_response m_header;
  std::shared_ptr<connection_pool> m_pool;
  std::string m_origin;
  std::chrono::milliseconds m_idle_timeout;
  bool m_finished = false;
};

/// A connection plus where it came from.
///
/// Only a connection taken from the pool is retried. A fresh connection that dies mid-exchange died
/// for a reason the server just expressed — a refused or rejected request is likelier than a broken
/// accept — and retrying would re-send a body the peer may already have acted on.
struct attempt {
  std::shared_ptr<connection> m_connection;
  bool m_reused = false;
};

struct acquired {
  std::shared_ptr<connection> m_connection;
  http_response m_header;
};

} // namespace

struct http_client::impl {
  asio::any_io_executor m_executor;
  http_client_options m_options;
  std::shared_ptr<connection_pool> m_pool;

  impl(asio::any_io_executor executor, http_client_options options)
      : m_executor{std::move(executor)}, m_options{options},
        m_pool{
            std::make_shared<connection_pool>(options.m_max_idle_per_origin, options.m_idle_ttl)} {}

  [[nodiscard]] boost::asio::awaitable<result<attempt>> acquire(parsed_url const& url,
                                                                std::string const& origin) {
    if (auto idle = m_pool->take(origin); idle) {
      co_return attempt{.m_connection = std::move(idle), .m_reused = true};
    }
    auto fresh = co_await connection::open(url, m_options.m_connect_timeout);
    if (!fresh) {
      co_return std::unexpected(fresh.error());
    }
    co_return attempt{.m_connection = std::move(*fresh), .m_reused = false};
  }

  [[nodiscard]] boost::asio::awaitable<result<acquired>>
  run(http_request const& request, std::chrono::milliseconds send_timeout,
      std::chrono::milliseconds header_timeout, std::size_t max_body_bytes) {
    auto url = parse_url(request.m_url);
    if (!url) {
      co_return std::unexpected(url.error());
    }
    auto const origin = origin_of(*url);

    for (int round = 0; round < 2; ++round) {
      auto current = co_await acquire(*url, origin);
      if (!current) {
        co_return std::unexpected(current.error());
      }
      auto& candidate = current->m_connection;
      auto sent = co_await candidate->send(*url, request, send_timeout);
      if (sent) {
        auto header = co_await candidate->read_header(max_body_bytes, header_timeout);
        if (header) {
          co_return acquired{.m_connection = std::move(candidate), .m_header = std::move(*header)};
        }
        bool const retryable = current->m_reused && !candidate->response_started();
        candidate->close();
        if (!retryable) {
          co_return std::unexpected(header.error());
        }
        continue;
      }
      bool const retryable = current->m_reused && !candidate->response_started();
      candidate->close();
      if (!retryable) {
        co_return std::unexpected(sent.error());
      }
    }
    co_return std::unexpected(
        stage_error(url->m_host, "read", "the server closed the pooled connection"));
  }
};

http_client::http_client(asio::io_context& io, http_client_options options)
    : m_impl{std::make_shared<impl>(io.get_executor(), options)} {}

http_client::~http_client() = default;

void http_client::close() {
  m_impl->m_pool->close();
}

boost::asio::awaitable<result<http_response>> http_client::send(http_request const& request) {
  budget const allowance{m_impl->m_options.m_request_timeout};
  auto outcome = co_await m_impl->run(request, allowance.remaining(), allowance.remaining(),
                                      m_impl->m_options.m_max_buffered_body);
  if (!outcome) {
    co_return std::unexpected(outcome.error());
  }
  auto& [connection, header] = *outcome;
  auto url = parse_url(request.m_url);
  if (!url) {
    co_return std::unexpected(url.error());
  }

  connection_stream stream{std::move(connection), std::move(header), m_impl->m_pool,
                           origin_of(*url), m_impl->m_options.m_stream_idle_timeout};
  http_response response;
  response.m_status = stream.status();
  response.m_headers = stream.headers();
  std::size_t const limit = m_impl->m_options.m_max_buffered_body;
  for (;;) {
    auto chunk = co_await stream.read_some();
    if (!chunk) {
      co_return std::unexpected(chunk.error());
    }
    if (!chunk->has_value()) {
      break;
    }
    if (response.m_body.size() + chunk->value().size() > limit) {
      co_return std::unexpected(
          stage_error(url->m_host, "read", "the response body is larger than allowed"));
    }
    response.m_body.append(chunk->value());
  }
  co_return response;
}
boost::asio::awaitable<result<std::unique_ptr<http_stream>>>
http_client::open(http_request const& request) {
  budget const allowance{m_impl->m_options.m_stream_idle_timeout};
  auto outcome = co_await m_impl->run(request, allowance.remaining(), allowance.remaining(), 0);
  if (!outcome) {
    co_return std::unexpected(outcome.error());
  }
  auto url = parse_url(request.m_url);
  if (!url) {
    co_return std::unexpected(url.error());
  }
  auto [connection, header] = std::move(*outcome);
  co_return std::unique_ptr<http_stream>{std::make_unique<connection_stream>(
      std::move(connection), std::move(header), m_impl->m_pool, origin_of(*url),
      m_impl->m_options.m_stream_idle_timeout)};
}

} // namespace qoderz::net