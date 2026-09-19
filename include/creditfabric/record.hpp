// Credit Fabric - durable record shapes and their codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The journal holds exactly two record kinds: a decided attempt, and a
// snapshot. Everything durable about an account - configuration, committed
// ledger, attempt window, incarnation standing, fences, epoch - is reachable
// from those two shapes.

#ifndef CREDITFABRIC_RECORD_HPP
#define CREDITFABRIC_RECORD_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "creditfabric/engine.hpp"
#include "creditfabric/status.hpp"

namespace creditfabric {

/// Durable, quiescent state of one account at a journal sequence boundary.
struct DurableAccount {
  AccountConfig config{};
  EpochId epoch{};
  Generation generation{};
  std::uint64_t capacity{0};
  CreditTotals totals{};
  bool hard_exhausted{false};
  Reason exhaustion_cause{Reason::Ok};
  Digest128 state_digest{};
  std::vector<IncarnationRecord> incarnations{};
  std::vector<FenceRecord> fences{};
  std::vector<AmbiguousNote> ambiguous{};
  std::uint64_t committed_attempts{0};
  std::uint64_t refused_attempts{0};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> attempt_window{};  ///< (incarnation, sequence)
};

/// Durable engine state. Produced by snapshot, consumed by recovery.
struct DurableState {
  std::uint64_t journal_sequence{0};
  Digest128 chain{};
  std::uint64_t snapshots{0};
  std::vector<DurableAccount> accounts{};
  Digest128 state_digest{};  ///< digest of every account, verified on restore
};

/// A decided attempt, as written to the journal.
struct DurableAttempt {
  CreditRequest request{};
  CreditOutcome outcome{};
  bool has_account{false};
  bool created_account{false};
  DurableAccount account{};
  std::uint64_t engine_applied_attempts{0};
  std::uint64_t engine_refused_attempts{0};
};

[[nodiscard]] Status encode_durable_attempt(const DurableAttempt& attempt, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_durable_attempt(const std::uint8_t* data, std::size_t size, DurableAttempt& out);

[[nodiscard]] Status encode_durable_state(const DurableState& state, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_durable_state(const std::uint8_t* data, std::size_t size, DurableState& out);

/// True when a refusal is decided against the ledger (and therefore consumes a
/// sequence number and is journaled), as opposed to a pre-admission rejection
/// of stale, unknown or malformed authority, which carries no durable identity.
[[nodiscard]] bool is_admitted_stage_refusal(Reason reason) noexcept;

}  // namespace creditfabric

#endif  // CREDITFABRIC_RECORD_HPP
