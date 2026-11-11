#include "protocol/body_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace qoderz::protocol {
namespace {

consteval std::array<std::int16_t, 256> build_lookup() {
  std::array<std::int16_t, 256> lookup{};
  lookup.fill(-1);
  for (std::size_t index = 0; index < body_alphabet.size(); ++index) {
    lookup[static_cast<unsigned char>(body_alphabet[index])] = static_cast<std::int16_t>(index);
  }
  return lookup;
}

inline constexpr auto body_lookup = build_lookup();

/// Swaps the first and the last third of the encoded run, leaving the middle in place.
void swap_thirds(std::span<char> encoded) noexcept {
  auto const third = encoded.size() / 3;
  if (third == 0) {
    return;
  }
  std::swap_ranges(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(third),
                   encoded.end() - static_cast<std::ptrdiff_t>(third));
}

/// Logical position inside the original, unswapped run.
constexpr std::size_t map_index(std::size_t logical, std::size_t length,
                                std::size_t third) noexcept {
  if (logical < third) {
    return logical + length - third;
  }
  return logical >= length - third ? logical - length + third : logical;
}

std::string hex_byte(unsigned char value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.push_back(digits[value >> 4U]);
  text.push_back(digits[value & 0x0FU]);
  return text;
}

} // namespace

std::size_t body_encode(std::string_view plain, std::span<char> destination) {
  // Explicit rather than assert: an undersized destination is a caller bug that must not become
  // out-of-bounds writes in a release build. Nothing is written, so the caller can only see a 0.
  if (destination.size() < max_encoded_length(plain.size())) {
    return 0;
  }
  std::size_t written = 0;
  std::size_t offset = 0;
  for (; offset + 3 <= plain.size(); offset += 3) {
    auto const block =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset + 1])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset + 2]));
    destination[written++] = body_alphabet[(block >> 18U) & 63U];
    destination[written++] = body_alphabet[(block >> 12U) & 63U];
    destination[written++] = body_alphabet[(block >> 6U) & 63U];
    destination[written++] = body_alphabet[block & 63U];
  }

  switch (plain.size() - offset) {
  case 1: {
    auto const block = static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset])) << 16U;
    destination[written++] = body_alphabet[(block >> 18U) & 63U];
    destination[written++] = body_alphabet[(block >> 12U) & 63U];
    destination[written++] = body_padding;
    destination[written++] = body_padding;
    break;
  }
  case 2: {
    auto const block =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(plain[offset + 1])) << 8U);
    destination[written++] = body_alphabet[(block >> 18U) & 63U];
    destination[written++] = body_alphabet[(block >> 12U) & 63U];
    destination[written++] = body_alphabet[(block >> 6U) & 63U];
    destination[written++] = body_padding;
    break;
  }
  default:
    break;
  }

  swap_thirds(destination.first(written));
  return written;
}

std::string body_encode(std::string_view plain) {
  std::string encoded(max_encoded_length(plain.size()), '\0');
  // The buffer is sized from the same formula the callee checks, so a zero here means a real bug.
  if (body_encode(plain, encoded) != encoded.size()) {
    encoded.clear();
  }
  return encoded;
}

result<std::string> body_decode(std::string_view encoded) {
  std::string plain;
  plain.reserve(max_decoded_length(encoded.size()));

  auto const third = encoded.size() / 3;
  for (std::size_t group = 0; group + 4 <= encoded.size(); group += 4) {
    std::uint32_t accumulator = 0;
    int count = 0;
    for (std::size_t slot = 0; slot < 4; ++slot) {
      auto const index = map_index(group + slot, encoded.size(), third);
      auto const value = static_cast<unsigned char>(encoded[index]);
      if (value == static_cast<unsigned char>(body_padding)) {
        continue;
      }
      auto const six_bits = body_lookup[value];
      if (six_bits < 0) {
        return std::unexpected(make_error(error_kind::protocol, "body codec: invalid character 0x" +
                                                                    hex_byte(value) + " at index " +
                                                                    std::to_string(index)));
      }
      accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(six_bits);
      ++count;
    }

    auto const bytes = static_cast<std::size_t>(6 * count / 8);
    for (std::size_t slot = 0; slot < bytes; ++slot) {
      auto const shift = static_cast<unsigned>(6 * count - 8 * static_cast<int>(slot + 1));
      plain.push_back(static_cast<char>((accumulator >> shift) & 0xFFU));
    }
  }

  return plain;
}

} // namespace qoderz::protocol
