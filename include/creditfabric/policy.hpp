// Credit Fabric - account configuration, profiles and engine limits.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef CREDITFABRIC_POLICY_HPP
#define CREDITFABRIC_POLICY_HPP

#include <cstdint>
#include <limits>
#include <string>

#include "creditfabric/authority.hpp"
#include "creditfabric/strong.hpp"

namespace creditfabric {

/// Provenance of a credit mechanism. Credit Fabric only ever claims what this
/// field can be defended as.
enum class ProfileKind : std::uint8_t {
  Abstract = 0,             ///< mechanism-neutral model; no physical claim
  Synthetic = 1,            ///< generated workload / simulation; no physical claim
  PhysicalUnvalidated = 2,  ///< models a physical protocol; NOT validated here
  PhysicalValidated = 3,    ///< would require hardware evidence; refused by this build
};

[[nodiscard]] const char* to_string(ProfileKind kind) noexcept;

/// Per-account credit policy. All fields are bounded; the engine refuses any
/// configuration word that is structurally invalid.
struct CreditPolicy {
  std::uint64_t min_issue{1};
  std::uint64_t max_issue_per_attempt{(std::numeric_limits<std::uint64_t>::max)()};
  bool allow_protect{true};
  bool allow_return{true};
  bool allow_capacity_growth{false};
};

/// Immutable account configuration established at open time.
struct AccountConfig {
  DomainId domain{};
  AccountId account{};
  ResourceId resource{};
  PolicyId policy{};
  std::uint64_t capacity{0};
  std::uint64_t max_capacity{0};
  ProfileKind profile{ProfileKind::Abstract};
  CreditPolicy rules{};
};

/// Structural bounds. Every bound exists so that a hostile or broken peer can
/// never make the authority allocate, log, or remember an unbounded amount.
struct EngineLimits {
  std::size_t max_accounts{64};
  std::size_t max_incarnations_per_account{256};
  std::size_t max_fences_per_account{1024};
  std::size_t max_attempt_window{8192};
  std::size_t max_ambiguous_attempts{256};
  std::size_t max_refusal_log{256};
  std::size_t max_explanation_incarnations{64};
  std::size_t max_explanation_fences{64};
  std::size_t max_explanation_refusals{32};
  std::size_t max_note_bytes{128};
  std::size_t max_journal_record_bytes{1u << 20};
  std::size_t max_frame_payload_bytes{1u << 16};
  std::size_t max_connections{64};
  std::uint64_t max_journal_bytes{64ull << 20};
  std::size_t journal_records_per_snapshot{4096};
};

[[nodiscard]] const EngineLimits& default_limits() noexcept;

}  // namespace creditfabric

#endif  // CREDITFABRIC_POLICY_HPP
