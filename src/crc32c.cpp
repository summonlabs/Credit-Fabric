// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/crc32c.hpp"

#include <array>

namespace creditfabric {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;

/// The lookup table is a plain array rather than a std::array so that every
/// index is provably inside its bounds for a static analyser as well as for a
/// human reader.
struct Table {
  std::uint32_t values[256];
};

constexpr Table make_table() noexcept {
  Table table{};
  for (std::size_t i = 0; i < 256u; ++i) {
    std::uint32_t crc = static_cast<std::uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ kPolynomial) : (crc >> 1);
    }
    table.values[i] = crc;
  }
  return table;
}

constexpr Table kTable = make_table();

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t count, std::uint32_t seed) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t index = static_cast<std::size_t>((crc ^ bytes[i]) & 0xFFu);
    crc = kTable.values[index] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c_u64(std::uint64_t value, std::uint32_t seed) noexcept {
  unsigned char buffer[8];
  for (int i = 0; i < 8; ++i) buffer[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  return crc32c(buffer, sizeof(buffer), seed);
}

}  // namespace creditfabric
