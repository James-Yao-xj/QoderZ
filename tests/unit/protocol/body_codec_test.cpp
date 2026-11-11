#include "protocol/body_codec.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct body_vector {
  std::string_view m_plain;
  std::string_view m_encoded;
};

// Copied verbatim from tests/NoQoder.Tests/Protocol/BodyCodecTests.cs; never regenerate.
constexpr body_vector body_vectors[] = {
    {"", ""},
    {"a", "$p$#"},
    {"ab", "$SB#"},
    {"abc", "MSK#"},
    {"hello", "q$FruHPH"},
    {"Qoder", "B$WzDxaS"},
    {"工具调用", "RQQ,n#SkyGoQumLQ"},
    {R"({"name":"Bash","arguments":{"command":"pwd"}})",
     "u(.t#SQzBMn*NZJzBE.WBOgYDkFtDSQwNYBymYKMmYKh#S.LBMn*pOg^PoBr"},
};

struct length_vector {
  std::size_t m_source;
  std::size_t m_encoded;
};

constexpr length_vector length_vectors[] = {
    {0, 0}, {1, 4}, {2, 4}, {3, 4}, {4, 8}, {5, 8}, {300, 400},
};

std::string deterministic_bytes(std::size_t length) {
  std::string bytes(length, '\0');
  std::uint32_t state = 0x2545F491U;
  for (auto& byte : bytes) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<char>(state >> 24U);
  }
  return bytes;
}

} // namespace

TEST_CASE("body codec encodes the fixed vectors", "[protocol][body_codec]") {
  for (auto const& vector : body_vectors) {
    CAPTURE(vector.m_plain);
    CHECK(qoderz::protocol::body_encode(vector.m_plain) == vector.m_encoded);
  }
}

TEST_CASE("body codec decodes the fixed vectors", "[protocol][body_codec]") {
  for (auto const& vector : body_vectors) {
    CAPTURE(vector.m_encoded);
    auto const decoded = qoderz::protocol::body_decode(vector.m_encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == vector.m_plain);
  }
}

TEST_CASE("body codec lengths pad to whole groups", "[protocol][body_codec]") {
  for (auto const& vector : length_vectors) {
    CHECK(qoderz::protocol::max_encoded_length(vector.m_source) == vector.m_encoded);
  }
  CHECK(qoderz::protocol::max_decoded_length(0) == 0);
  CHECK(qoderz::protocol::max_decoded_length(4) == 3);
  CHECK(qoderz::protocol::max_decoded_length(8) == 6);
  CHECK(qoderz::protocol::max_decoded_length(400) == 300);
}

TEST_CASE("body codec round-trips every length up to three hundred", "[protocol][body_codec]") {
  auto const source = deterministic_bytes(300);
  for (std::size_t length = 0; length <= source.size(); ++length) {
    auto const slice = std::string_view{source}.substr(0, length);
    auto const encoded = qoderz::protocol::body_encode(slice);
    CAPTURE(length);
    REQUIRE(encoded.size() == qoderz::protocol::max_encoded_length(length));
    REQUIRE(encoded.size() % 4 == 0);

    auto const decoded = qoderz::protocol::body_decode(encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == slice);
  }
}

TEST_CASE("body codec writes the full destination", "[protocol][body_codec]") {
  // 0x00 is outside the alphabet and is not '$', so any surviving 0x00 proves the destination was
  // not written through.
  for (std::size_t length = 0; length <= 64; ++length) {
    auto const source = deterministic_bytes(length);
    std::string destination(qoderz::protocol::max_encoded_length(length), '\0');

    auto const written = qoderz::protocol::body_encode(source, destination);
    CAPTURE(length);
    CHECK(written == qoderz::protocol::max_encoded_length(length));
    CHECK(written == destination.size());
    CHECK(destination.find('\0') == std::string::npos);
    CHECK(std::count(destination.begin(), destination.end(), '$') == (length % 3 == 1   ? 2
                                                                      : length % 3 == 2 ? 1
                                                                                        : 0));

    auto const decoded = qoderz::protocol::body_decode(destination);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == source);
  }
}

TEST_CASE("body codec refuses an undersized destination", "[protocol][body_codec]") {
  // Contract: a destination smaller than `max_encoded_length` gets nothing written and returns 0.
  for (std::size_t length = 1; length <= 32; ++length) {
    auto const source = deterministic_bytes(length);
    auto const needed = qoderz::protocol::max_encoded_length(length);
    CAPTURE(length);

    for (std::size_t size = 0; size < needed; ++size) {
      // A sentinel that is not in the alphabet, so any write is visible.
      std::string destination(size, '?');
      auto const before = destination;

      auto const written = qoderz::protocol::body_encode(source, destination);
      CAPTURE(size);
      CHECK(written == 0);
      CHECK(destination == before);
    }

    std::string exact(needed, '?');
    CHECK(qoderz::protocol::body_encode(source, exact) == needed);
    CHECK(qoderz::protocol::body_decode(exact) == source);
  }

  // Empty input encodes to nothing whether the destination is empty or merely small.
  std::string empty_destination;
  CHECK(qoderz::protocol::body_encode("", empty_destination) == 0);
  std::string tiny(1, '?');
  CHECK(qoderz::protocol::body_encode("abc", tiny) == 0);
  CHECK(tiny == "?");
}

TEST_CASE("body codec padding marks trailing bytes", "[protocol][body_codec]") {
  auto const count_padding = [](std::string_view encoded) {
    return std::count(encoded.begin(), encoded.end(), '$');
  };

  CHECK(count_padding(qoderz::protocol::body_encode("abc")) == 0);
  // Four bytes are one whole group plus a one-byte tail: two padding characters.
  CHECK(count_padding(qoderz::protocol::body_encode("abcd")) == 2);
  CHECK(count_padding(qoderz::protocol::body_encode("a")) == 2);
  CHECK(count_padding(qoderz::protocol::body_encode("ab")) == 1);
}

TEST_CASE("body codec rejects characters outside the alphabet", "[protocol][body_codec]") {
  for (std::string_view const encoded : {"abc=", "MS K", "MSK ", "工具调用"}) {
    CAPTURE(encoded);
    auto const decoded = qoderz::protocol::body_decode(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().m_kind == qoderz::error_kind::protocol);
  }
}

TEST_CASE("body codec ignores a trailing partial group", "[protocol][body_codec]") {
  // A partial group adds no bytes. The de-swap offset follows the total length, so the result is
  // not required to equal the four-character decode.
  auto const whole = qoderz::protocol::body_decode("MSK#");
  auto const partial = qoderz::protocol::body_decode("MSK#x");
  REQUIRE(whole.has_value());
  REQUIRE(partial.has_value());
  CHECK(whole->size() == 3);
  CHECK(partial->size() == 3);
  CHECK(*whole == "abc");
  // The reference de-swaps from the total length, so a fifth character also shifts the read
  // order of the first group instead of being a clean no-op.
  CHECK(*partial == "]bX");
}

TEST_CASE("body codec round-trips utf8 text", "[protocol][body_codec]") {
  // The nul byte needs an explicit length; a literal would stop at the terminator.
  std::vector<std::string> const cases{
      std::string{},
      std::string{"hello world"},
      std::string{"Qoder 工具调用 — 你好，世界"},
      std::string{"line\nbreak\r\nand \0 nul", 21},
  };

  for (auto const& text : cases) {
    auto const encoded = qoderz::protocol::body_encode(text);
    auto const decoded = qoderz::protocol::body_decode(encoded);
    CAPTURE(text);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == text);
  }
}
