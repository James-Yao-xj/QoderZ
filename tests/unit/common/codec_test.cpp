#include "common/codec/base64.hpp"
#include "common/codec/hash.hpp"
#include "common/codec/random.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::span<std::byte const> bytes_of(std::string_view text) {
  return std::as_bytes(std::span{text});
}

bool is_lower_hex(std::string_view text) {
  for (char const character : text) {
    auto const is_digit = character >= '0' && character <= '9';
    auto const is_letter = character >= 'a' && character <= 'f';
    if (!is_digit && !is_letter) {
      return false;
    }
  }
  return true;
}

struct base64_case {
  std::string_view m_plain;
  std::string_view m_encoded;
};

constexpr base64_case base64_cases[] = {
    {"", ""},
    {"f", "Zg=="},
    {"fo", "Zm8="},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg=="},
    {"fooba", "Zm9vYmE="},
    {"foobar", "Zm9vYmFy"},
};

constexpr base64_case base64url_cases[] = {
    {"", ""},           {"f", "Zg"},          {"fo", "Zm8"},          {"foo", "Zm9v"},
    {"foob", "Zm9vYg"}, {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"},
};

} // namespace

TEST_CASE("base64 encodes the RFC 4648 vectors", "[common][codec][base64]") {
  for (auto const& test : base64_cases) {
    CHECK(qoderz::codec::base64_encode(test.m_plain) == test.m_encoded);
  }
}

TEST_CASE("base64 decodes the RFC 4648 vectors", "[common][codec][base64]") {
  for (auto const& test : base64_cases) {
    auto const decoded = qoderz::codec::base64_decode(test.m_encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == test.m_plain);
  }
}

TEST_CASE("base64url uses the url alphabet and drops padding", "[common][codec][base64]") {
  for (auto const& test : base64url_cases) {
    CHECK(qoderz::codec::base64url_encode(bytes_of(test.m_plain)) == test.m_encoded);

    auto const decoded = qoderz::codec::base64url_decode(test.m_encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == test.m_plain);
  }
}

TEST_CASE("base64url keeps padding when asked", "[common][codec][base64]") {
  CHECK(qoderz::codec::base64url_encode(bytes_of("f"), true) == "Zg==");
  CHECK(qoderz::codec::base64url_encode(bytes_of("fo"), true) == "Zm8=");
  CHECK(qoderz::codec::base64url_encode(bytes_of("foo"), true) == "Zm9v");
}

TEST_CASE("base64 alphabets diverge on non-ascii bytes", "[common][codec][base64]") {
  auto const data = std::string{"\xfb\xff", 2};
  // 0xFB 0xFF is the standard "+/8=" and the url "-_8=".
  CHECK(qoderz::codec::base64_encode(data) == "+/8=");
  CHECK(qoderz::codec::base64url_encode(bytes_of(data)) == "-_8");
  CHECK(qoderz::codec::base64url_encode(bytes_of(data), true) == "-_8=");
}

TEST_CASE("base64 rejects malformed input", "[common][codec][base64]") {
  for (std::string_view const text : {"QQ=", "Q", "****", "QQ===", "QQ=A", "a b c"}) {
    auto const decoded = qoderz::codec::base64_decode(text);
    CAPTURE(text);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().m_kind == qoderz::error_kind::protocol);
  }
}

TEST_CASE("base64 round-trips every length up to eight", "[common][codec][base64]") {
  auto const source = std::string{"0123456789"};
  for (std::size_t length = 0; length <= 8; ++length) {
    auto const slice = source.substr(0, length);
    auto const encoded = qoderz::codec::base64_encode(slice);
    CHECK(encoded.size() % 4 == 0);
    auto const decoded = qoderz::codec::base64_decode(encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == slice);
  }
}

TEST_CASE("sha256 matches the fixed vectors", "[common][codec][hash]") {
  CHECK(qoderz::codec::sha256_hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(qoderz::codec::sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(qoderz::codec::sha256_hex("hello world") ==
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_CASE("sha256 streams like the one-shot hash", "[common][codec][hash]") {
  constexpr std::string_view sentence = "The quick brown fox jumps over the lazy dog";
  constexpr std::string_view expected =
      "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592";

  qoderz::codec::sha256 hasher;
  for (std::size_t offset = 0; offset < sentence.size(); offset += 7) {
    hasher.update(sentence.substr(offset, 7));
  }
  CHECK(hasher.hex_digest() == expected);

  // The digest is final: a second call returns the cached value and further updates are ignored.
  hasher.update("ignored");
  CHECK(hasher.hex_digest() == expected);
  CHECK(qoderz::codec::sha256_hex(sentence) == expected);
}

TEST_CASE("md5 matches the fixed vectors", "[common][codec][hash]") {
  CHECK(qoderz::codec::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
  CHECK(qoderz::codec::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
  CHECK(qoderz::codec::md5_hex("hello world") == "5eb63bbbe01eeed093cb22bb8f5acdc3");
  CHECK(qoderz::codec::md5_hex("The quick brown fox jumps over the lazy dog") ==
        "9e107d9d372bb6826bd81d3542a419d6");
}

TEST_CASE("md5 hashes disjoint binary parts as their concatenation", "[common][codec][hash]") {
  auto const binary = std::string{"a\0b\xff", 4};
  std::array<std::string_view, 4> const parts{std::string_view{binary}.substr(0, 2), "",
                                              std::string_view{binary}.substr(2), "suffix"};
  CHECK(qoderz::codec::md5_hex_parts(parts) == qoderz::codec::md5_hex(binary + "suffix"));
  CHECK(qoderz::codec::md5_hex_parts({}) == qoderz::codec::md5_hex(""));
}

TEST_CASE("hex_encode writes lowercase pairs", "[common][codec][hash]") {
  CHECK(qoderz::codec::hex_encode({}).empty());
  auto const data = std::string{"\x00\x0f\xff\x10", 4};
  CHECK(qoderz::codec::hex_encode(bytes_of(data)) == "000fff10");
}

TEST_CASE("random bytes are sized and unpredictable", "[common][codec][random]") {
  CHECK(qoderz::codec::random_bytes(0).empty());
  CHECK(qoderz::codec::random_bytes(8).size() == 8);

  auto const first = qoderz::codec::random_bytes(32);
  auto const second = qoderz::codec::random_bytes(32);
  CHECK(first.size() == 32);
  CHECK(first != second);
}

TEST_CASE("random_hex yields 2n lowercase hex characters", "[common][codec][random]") {
  auto const text = qoderz::codec::random_hex(16);
  CHECK(text.size() == 32);
  CHECK(is_lower_hex(text));
  CHECK(qoderz::codec::random_hex(0).empty());
  CHECK(qoderz::codec::random_hex(8) != qoderz::codec::random_hex(8));
}

TEST_CASE("random_base64url is unpadded and decodable", "[common][codec][random]") {
  auto const text = qoderz::codec::random_base64url(32);
  CHECK(text.size() == 43);
  CHECK(text.find('=') == std::string::npos);
  CHECK(text.find('+') == std::string::npos);
  CHECK(text.find('/') == std::string::npos);

  auto const decoded = qoderz::codec::base64url_decode(text);
  REQUIRE(decoded.has_value());
  CHECK(decoded->size() == 32);
}

TEST_CASE("base64 rejects the url alphabet", "[common][codec][base64]") {
  CHECK_FALSE(qoderz::codec::base64_decode("-_8").has_value());
  CHECK_FALSE(qoderz::codec::base64_decode("-_-_").has_value());
  CHECK(qoderz::codec::base64_decode("+/8=").has_value());

  auto const url_form = qoderz::codec::base64url_decode("-_8");
  REQUIRE(url_form.has_value());
  CHECK(url_form->size() == 2);
}

TEST_CASE("base64 accepts unpadded input", "[common][codec][base64]") {
  auto const short_form = qoderz::codec::base64_decode("Zg");
  REQUIRE(short_form.has_value());
  CHECK(*short_form == "f");

  auto const longer = qoderz::codec::base64_decode("Zm9vYmE");
  REQUIRE(longer.has_value());
  CHECK(*longer == "fooba");
}

TEST_CASE("sha256 handles the block padding boundaries", "[common][codec][hash]") {
  // 56 is the first length that needs an extra block; 55 and 64 sit either side of it.
  CHECK(qoderz::codec::sha256_hex(std::string(55, 'a')) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(qoderz::codec::sha256_hex(std::string(56, 'a')) ==
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(qoderz::codec::sha256_hex(std::string(64, 'a')) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
