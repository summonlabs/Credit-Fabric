// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/version.hpp"

namespace creditfabric {

const Version& version() noexcept {
  static const Version instance{};
  return instance;
}

std::string version_string() {
  return std::to_string(CREDITFABRIC_VERSION_MAJOR) + "." + std::to_string(CREDITFABRIC_VERSION_MINOR) + "." +
         std::to_string(CREDITFABRIC_VERSION_PATCH);
}

const char* build_profile() noexcept {
#if defined(NDEBUG)
  return "release";
#else
  return "debug";
#endif
}

}  // namespace creditfabric
