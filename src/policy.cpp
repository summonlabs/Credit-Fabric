// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/policy.hpp"

namespace creditfabric {

const char* to_string(ProfileKind kind) noexcept {
  switch (kind) {
    case ProfileKind::Abstract:
      return "ABSTRACT";
    case ProfileKind::Synthetic:
      return "SYNTHETIC";
    case ProfileKind::PhysicalUnvalidated:
      return "PHYSICAL-UNVALIDATED";
    case ProfileKind::PhysicalValidated:
      return "PHYSICAL-VALIDATED";
  }
  return "UNKNOWN";
}

const EngineLimits& default_limits() noexcept {
  static const EngineLimits limits{};
  return limits;
}

}  // namespace creditfabric
