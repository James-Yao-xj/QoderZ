#include "server/endpoints/chat.hpp"

#include "common/log.hpp"
#include "common/strings.hpp"
#include "openai/response/response.hpp"
#include "openai/response/stream_frames.hpp"
#include "protocol/sse/chat_event.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::server::endpoints {
namespace {

constexpr std::string_view invalid_json_message = "the request body is not valid JSON";
constexpr std::string_view missing_messages_message = "messages is required";
constexpr std::string_view missing_model_message = "model is required";
constexpr int fallback_error_status = 502;

struct parsed_chat {
  json m_body;
  std::string m_model;
  bool m_stream = false;
};

/// Strips the optional `"{site}/"` prefix, which lets one client address several sites by model
/// name.
void strip_site_prefix(std::string& model, std::string_view site_id) {
  if (site_id.empty() || model.size() <= site_id.size() || model[site_id.size()] != '/') {
    return;
  }
  if (!strings::equals_ignore_case(std::string_view{model}.substr(0, site_id.size()), site_id)) {
    return;
  }
  model = strings::trim(std::string_view{model}.substr(site_id.size() + 1));
}

result<parsed_chat> parse_chat(chat_http_request& request, std::string_view site_id) {
  auto body = json::parse(request.m_body, nullptr, false);
  std::string{}.swap(request.m_body);
  if (body.is_discarded()) {
    return std::unexpected(bad_request_error(std::string{invalid_json_message}));
  }

  auto const messages = body.find("messages");
  if (messages == body.end() || !messages->is_array()) {
    return std::unexpected(bad_request_error(std::string{missing_messages_message}));
  }

  auto const model = body.find("model");
  if (model == body.end() || !model->is_string()) {
    return std::unexpected(bad_request_error(std::string{missing_model_message}));
  }
  auto name = std::string{strings::trim(model->get_ref<std::string const&>())};
  strip_site_prefix(name, site_id);
  if (name.empty()) {
    return std::unexpected(bad_request_error(std::string{missing_model_message}));
  }

  auto const flag = body.find("stream");
  auto const stream = flag != body.end() && flag->is_boolean() && flag->get<bool>();

  parsed_chat parsed;
  parsed.m_body = std::move(body);
  parsed.m_model = std::move(name);
  parsed.m_stream = stream;
  return parsed;
}

error canceled_failure(error failure) {
  if (failure.m_kind == error_kind::canceled) {
    return failure;
  }
  return make_error(error_kind::canceled, std::move(failure.m_message));
}

/// A transport failure as the event the frame translator knows how to render, so an error that
/// happens after the first frame still leaves the stream in one shape.
protocol::chat_event transport_event(error failure) {
  protocol::chat_event event;
  event.m_kind = protocol::chat_event_kind::error;
  event.m_status = failure.m_http_status > 0 ? failure.m_http_status : fallback_error_status;
  event.m_error = std::move(failure.m_message);
  return event;
}

boost::asio::awaitable<result<void>> write_frames(response_sink& sink,
                                                  std::vector<std::string> const& frames) {
  for (auto const& frame : frames) {
    auto written = co_await sink.write_sse(frame);
    if (!written) {
      co_return std::unexpected(canceled_failure(std::move(written.error())));
    }
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> stream_answer(detail::opened_upstream& opened,
                                                   openai::selection::session_store& sessions,
                                                   clock& time, response_sink& sink) {
  auto begun = co_await sink.begin_sse();
  if (!begun) {
    co_return begun;
  }

  openai::response_identity const identity{
      .m_id = openai::make_completion_id(),
      .m_created = static_cast<std::int64_t>(time.now_ms() / 1000),
      .m_model = opened.m_model_key,
  };
  openai::stream_translator translator{identity};

  for (;;) {
    auto event = co_await opened.m_stream->next();
    if (!event) {
      auto frames = translator.on_event(transport_event(std::move(event.error())));
      auto sent = co_await write_frames(sink, frames);
      if (!sent) {
        co_return sent;
      }
      auto ended = co_await sink.end_sse();
      if (!ended) {
        co_return ended;
      }
      co_return result<void>{};
    }
    if (!event->has_value()) {
      break;
    }

    auto frames = translator.on_event(**event);
    auto sent = co_await write_frames(sink, frames);
    if (!sent) {
      co_return sent;
    }
    if (translator.failed()) {
      auto ended = co_await sink.end_sse();
      if (!ended) {
        co_return ended;
      }
      co_return result<void>{};
    }
    if (translator.answered()) {
      break;
    }
  }

  auto tail = translator.finish();
  if (!tail.empty()) {
    auto sent = co_await write_frames(sink, std::vector<std::string>{std::move(tail)});
    if (!sent) {
      co_return sent;
    }
  }
  auto ended = co_await sink.end_sse();
  // §5.5: the turn only counts as answered once the downstream is completely written. A client
  // that dies between the Stop frame and the terminating chunk leaves it unanswered, so its next
  // request opens a turn instead of continuing one.
  if (!ended) {
    co_return ended;
  }
  if (translator.answered()) {
    sessions.mark_answered(opened.m_task_id);
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<void>> json_answer(detail::opened_upstream& opened,
                                                 openai::selection::session_store& sessions,
                                                 clock& time, response_sink& sink) {
  std::vector<protocol::chat_event> events;
  for (;;) {
    auto event = co_await opened.m_stream->next();
    if (!event) {
      co_return std::unexpected(std::move(event.error()));
    }
    if (!event->has_value()) {
      break;
    }
    auto const kind = event->value().m_kind;
    events.push_back(std::move(**event));
    if (kind == protocol::chat_event_kind::stop || kind == protocol::chat_event_kind::error) {
      break;
    }
  }

  openai::response_identity const identity{
      .m_id = openai::make_completion_id(),
      .m_created = static_cast<std::int64_t>(time.now_ms() / 1000),
      .m_model = opened.m_model_key,
  };
  auto completion = openai::build_completion(events, identity);
  if (!completion) {
    co_return std::unexpected(std::move(completion.error()));
  }

  auto written = co_await sink.write_json(200, std::move(completion->m_json));
  if (!written) {
    co_return written;
  }
  if (completion->m_answered) {
    sessions.mark_answered(opened.m_task_id);
  }
  co_return result<void>{};
}

} // namespace

chat_handler_impl::chat_handler_impl(account_pool& pool, qoder::catalog::catalog_service& catalog,
                                     qoder::auth::token_service& tokens,
                                     qoder::chat::chat_client& client,
                                     openai::selection::session_store& sessions, clock& time,
                                     std::string site_id)
    : m_pool{pool}, m_catalog{catalog}, m_tokens{tokens}, m_client{client}, m_sessions{sessions},
      m_time{time}, m_site_id{std::move(site_id)} {}

boost::asio::awaitable<result<void>> chat_handler_impl::handle(chat_http_request& request,
                                                               response_sink& sink) {
  auto parsed = parse_chat(request, m_site_id);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed.error()));
  }

  auto opened = co_await detail::open_upstream(m_pool, m_catalog, m_tokens, m_client, m_sessions,
                                               m_time, std::move(parsed->m_body), parsed->m_model,
                                               request.m_session_id);
  if (!opened) {
    co_return std::unexpected(std::move(opened.error()));
  }

  log(log_level::debug, "chat request opened",
      {{"model", opened->m_model_key},
       {"stream", parsed->m_stream ? "true" : "false"},
       {"site", m_pool.site_id()}});

  if (parsed->m_stream) {
    co_return co_await stream_answer(*opened, m_sessions, m_time, sink);
  }
  co_return co_await json_answer(*opened, m_sessions, m_time, sink);
}

} // namespace qoderz::server::endpoints
