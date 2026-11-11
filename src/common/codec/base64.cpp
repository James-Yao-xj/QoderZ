#include "common/codec/base64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace qoderz::codec {
namespace {

consteval std::array<std::int16_t, 256> build_lookup(std::string_view alphabet) {
  std::array<std::int16_t, 256> lookup{};
  lookup.fill(-1);
  for (std::size_t index = 0; index < alphabet.size(); ++index) {
    lookup[static_cast<unsigned char>(alphabet[index])] = static_cast<std::int16_t>(index);
  }
  return lookup;
}

inline constexpr auto base64_lookup = build_lookup(base64_alphabet);
inline constexpr auto base64url_lookup = build_lookup(base64url_alphabet);

std::string encode(std::span<std::byte const> data, std::string_view alphabet, bool padding) {
  std::string output;
  output.reserve((data.size() / 3) * 4 + 4);

  std::size_t offset = 0;
  for (; offset + 3 <= data.size(); offset += 3) {
    auto const block = (std::to_integer<std::uint32_t>(data[offset]) << 16U) |
                       (std::to_integer<std::uint32_t>(data[offset + 1]) << 8U) |
                       std::to_integer<std::uint32_t>(data[offset + 2]);
    output.push_back(alphabet[(block >> 18U) & 63U]);
    output.push_back(alphabet[(block >> 12U) & 63U]);
    output.push_back(alphabet[(block >> 6U) & 63U]);
    output.push_back(alphabet[block & 63U]);
  }

  switch (data.size() - offset) {
  case 1: {
    auto const block = std::to_integer<std::uint32_t>(data[offset]) << 16U;
    output.push_back(alphabet[(block >> 18U) & 63U]);
    output.push_back(alphabet[(block >> 12U) & 63U]);
    if (padding) {
      output.push_back(base64_padding);
      output.push_back(base64_padding);
    }
    break;
  }
  case 2: {
    auto const block = (std::to_integer<std::uint32_t>(data[offset]) << 16U) |
                       (std::to_integer<std::uint32_t>(data[offset + 1]) << 8U);
    output.push_back(alphabet[(block >> 18U) & 63U]);
    output.push_back(alphabet[(block >> 12U) & 63U]);
    output.push_back(alphabet[(block >> 6U) & 63U]);
    if (padding) {
      output.push_back(base64_padding);
    }
    break;
  }
  default:
    break;
  }

  return output;
}

std::string hex_byte(unsigned char value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.push_back(digits[value >> 4U]);
  text.push_back(digits[value & 0x0FU]);
  return text;
}

error invalid_character(unsigned char value, std::size_t index) {
  return make_error(error_kind::protocol, "base64: invalid character 0x" + hex_byte(value) +
                                              " at index " + std::to_string(index));
}

result<std::string> decode(std::string_view text, std::array<std::int16_t, 256> const& lookup) {
  std::size_t length = text.size();
  std::size_t padding = 0;
  while (length > 0 && text[length - 1] == base64_padding) {
    --length;
    ++padding;
  }
  if (padding > 2) {
    return std::unexpected(make_error(error_kind::protocol, "base64: too much padding"));
  }
  if (padding > 0 && text.size() % 4 != 0) {
    return std::unexpected(
        make_error(error_kind::protocol, "base64: padded input is not a multiple of 4"));
  }
  if (length % 4 == 1) {
    return std::unexpected(make_error(error_kind::protocol, "base64: impossible input length"));
  }

  std::string output;
  output.reserve((length / 4) * 3 + 2);

  std::uint32_t accumulator = 0;
  int count = 0;
  for (std::size_t index = 0; index < length; ++index) {
    auto const raw = static_cast<unsigned char>(text[index]);
    auto const six_bits = lookup[raw];
    if (six_bits < 0) {
      return std::unexpected(invalid_character(raw, index));
    }
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(six_bits);
    ++count;
    if (count == 4) {
      output.push_back(static_cast<char>((accumulator >> 16U) & 0xFFU));
      output.push_back(static_cast<char>((accumulator >> 8U) & 0xFFU));
      output.push_back(static_cast<char>(accumulator & 0xFFU));
      accumulator = 0;
      count = 0;
    }
  }

  if (count == 2) {
    output.push_back(static_cast<char>((accumulator >> 4U) & 0xFFU));
  } else if (count == 3) {
    output.push_back(static_cast<char>((accumulator >> 10U) & 0xFFU));
    output.push_back(static_cast<char>((accumulator >> 2U) & 0xFFU));
  }

  return output;
}

} // namespace

std::string base64_encode(std::span<std::byte const> data) {
  return encode(data, base64_alphabet, true);
}

std::string base64_encode(std::string_view data) {
  return encode(std::as_bytes(std::span{data}), base64_alphabet, true);
}

std::string base64url_encode(std::span<std::byte const> data, bool padding) {
  return encode(data, base64url_alphabet, padding);
}

result<std::string> base64_decode(std::string_view text) {
  return decode(text, base64_lookup);
}

result<std::string> base64url_decode(std::string_view text) {
  return decode(text, base64url_lookup);
}

} // namespace qoderz::codec
