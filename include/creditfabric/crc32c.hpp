// Credit Fabric - CRC-32C (Castagnoli) integrity primitive.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef CREDITFABRIC_CRC32C_HPP
#define CREDITFABRIC_CRC32C_HPP

#include <cstddef>
#include <cstdint>

namespace creditfabric {

/// Reflected CRC-32C over the supplied bytes, continuing from \p seed.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t count, std::uint32_t seed = 0u) noexcept;

/// CRC-32C over a single unsigned 64-bit value in little-endian order.
[[nodiscard]] std::uint32_t crc32c_u64(std::uint64_t value, std::uint32_t seed = 0u) noexcept;

}  // namespace creditfabric

#endif  // CREDITFABRIC_CRC32C_HPP
