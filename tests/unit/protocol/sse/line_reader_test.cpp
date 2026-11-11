#include "protocol/sse/line_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qoderz::protocol::sse_line_reader;

std::vector<std::string> feed_all(std::string_view bytes, std::size_t chunk_size) {
  sse_line_reader reader;
  std::vector<std::string> lines;
  for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_size) {
    auto const produced = reader.feed(bytes.substr(offset, chunk_size));
    REQUIRE(produced.has_value());
    lines.insert(lines.end(), produced->begin(), produced->end());
  }
  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  lines.insert(lines.end(), tail->begin(), tail->end());
  return lines;
}

} // namespace

TEST_CASE("line reader splits on newlines", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed("data: one\n\ndata: two\n");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"data: one", "", "data: two"});

  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  CHECK(tail->empty());
}

TEST_CASE("line reader strips one carriage return", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed("data: one\r\n\r\ndata: two\r\n");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"data: one", "", "data: two"});

  auto const double_cr = reader.feed("a\r\r\n");
  REQUIRE(double_cr.has_value());
  CHECK(*double_cr == std::vector<std::string>{"a\r"});
}

TEST_CASE("line reader survives one byte per feed", "[protocol][sse][line_reader]") {
  CHECK(feed_all("data: one\r\n\r\ndata: two\r\n", 1) ==
        std::vector<std::string>{"data: one", "", "data: two"});
}

TEST_CASE("line reader delivers the last line without a newline", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed("data: one\ndata: two");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"data: one"});

  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  CHECK(*tail == std::vector<std::string>{"data: two"});

  auto const empty = reader.finish();
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("line reader delivers adjacent empty lines", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed("\n\nx");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"", ""});

  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  CHECK(*tail == std::vector<std::string>{"x"});
}

TEST_CASE("line reader keeps an empty trailing line marker", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed("x\n");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"x"});

  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  CHECK(tail->empty());
}

TEST_CASE("line reader carries a long line", "[protocol][sse][line_reader]") {
  auto const payload = std::string(512 * 1024, 'x');
  auto const data = "data: " + payload + "\nend\n";

  auto const lines = feed_all(data, 4096);
  REQUIRE(lines.size() == 2);
  CHECK(lines[0].size() == payload.size() + 6);
  CHECK(lines[1] == "end");
}

TEST_CASE("line reader accepts a line exactly at the limit", "[protocol][sse][line_reader]") {
  auto const data = std::string(sse_line_reader::max_line_bytes, 'x') + "\n";

  auto const lines = feed_all(data, 1 << 20);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].size() == sse_line_reader::max_line_bytes);
}

TEST_CASE("line reader measures the limit after stripping the carriage return",
          "[protocol][sse][line_reader]") {
  // A final line that never saw `\n`: the `\r` is part of the terminator, not of the delivered
  // line.
  sse_line_reader reader;
  auto const pending = reader.feed(std::string(sse_line_reader::max_line_bytes, 'x') + "\r");
  REQUIRE(pending.has_value());
  CHECK(pending->empty());

  auto const tail = reader.finish();
  REQUIRE(tail.has_value());
  REQUIRE(tail->size() == 1);
  CHECK((*tail)[0].size() == sse_line_reader::max_line_bytes);
  CHECK((*tail)[0].find('\r') == std::string::npos);

  // One byte over the limit is still rejected; the allowance is exactly the carriage return.
  sse_line_reader over;
  auto const overflow = over.feed(std::string(sse_line_reader::max_line_bytes + 1, 'x'));
  REQUIRE(overflow.has_value());
  auto const failed = over.finish();
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().m_kind == qoderz::error_kind::protocol);
}

TEST_CASE("line reader fails once a line grows past the limit", "[protocol][sse][line_reader]") {
  sse_line_reader reader;

  auto const pending = reader.feed(std::string(sse_line_reader::max_line_bytes + 1, 'x'));
  REQUIRE(pending.has_value());
  CHECK(pending->empty());

  auto const overflow = reader.feed("x");
  REQUIRE_FALSE(overflow.has_value());
  CHECK(overflow.error().m_kind == qoderz::error_kind::protocol);

  auto const again = reader.feed("\n");
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().m_kind == qoderz::error_kind::protocol);

  auto const finished = reader.finish();
  REQUIRE_FALSE(finished.has_value());
  CHECK(finished.error().m_kind == qoderz::error_kind::protocol);
}

TEST_CASE("line reader fails on a completed over-long line", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  auto const lines = reader.feed(std::string(sse_line_reader::max_line_bytes + 1, 'x') + "\n");
  REQUIRE_FALSE(lines.has_value());
  CHECK(lines.error().m_kind == qoderz::error_kind::protocol);
}

TEST_CASE("line reader reset clears buffered and failed state", "[protocol][sse][line_reader]") {
  sse_line_reader reader;
  REQUIRE(reader.feed("stale").has_value());
  REQUIRE_FALSE(reader.feed(std::string(sse_line_reader::max_line_bytes + 2, 'x')).has_value());

  reader.reset();

  auto const lines = reader.feed("fresh\n");
  REQUIRE(lines.has_value());
  CHECK(*lines == std::vector<std::string>{"fresh"});
}
