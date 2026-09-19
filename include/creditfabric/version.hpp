// Credit Fabric - vendor-neutral credit-based flow-control authority runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Version identity. The major version is frozen for the 1.x authority ABI.

#ifndef CREDITFABRIC_VERSION_HPP
#define CREDITFABRIC_VERSION_HPP

#include <cstdint>
#include <string>

#define CREDITFABRIC_VERSION_MAJOR 1
#define CREDITFABRIC_VERSION_MINOR 0
#define CREDITFABRIC_VERSION_PATCH 0

namespace creditfabric {

/// Durable journal / wire format revision understood by this build.
inline constexpr std::uint32_t kFormatVersion = 1u;

struct Version {
  std::uint32_t major{CREDITFABRIC_VERSION_MAJOR};
  std::uint32_t minor{CREDITFABRIC_VERSION_MINOR};
  std::uint32_t patch{CREDITFABRIC_VERSION_PATCH};
};

[[nodiscard]] const Version& version() noexcept;
[[nodiscard]] std::string version_string();
[[nodiscard]] const char* build_profile() noexcept;

}  // namespace creditfabric

#endif  // CREDITFABRIC_VERSION_HPP
