// Credit Fabric - authority vectors, incarnations, epochs and fences.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Authority is never a bare boolean. A decision is bound to the exact domain,
// account, epoch, generation and incarnation that justified it. Advancing any
// component invalidates every decision taken under the previous value.

#ifndef CREDITFABRIC_AUTHORITY_HPP
#define CREDITFABRIC_AUTHORITY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/status.hpp"
#include "creditfabric/strong.hpp"

namespace creditfabric {

/// Lifecycle of a process incarnation as known to this authority.
///
/// Unknown is a first-class state: an incarnation that has never reconciled in
/// the current epoch cannot authorize anything, and a restarted authority never
/// restores liveness from durable bytes.
enum class AuthorityState : std::uint8_t {
  Unknown = 0,       ///< never reconciled, or reconciled under a superseded epoch
  Reconciled = 1,    ///< proved liveness for the current epoch/generation
  Fenced = 2,        ///< permanently invalidated; never returns to a live state
  Retired = 3,       ///< closed by operator action; may be re-opened by reconciliation
};

[[nodiscard]] const char* to_string(AuthorityState state) noexcept;

/// A process incarnation: a publisher identity bound to one boot.
struct Incarnation {
  PublisherId publisher{};
  BootId boot{};
  IncarnationId id{};

  [[nodiscard]] static Incarnation make(PublisherId publisher, BootId boot) noexcept {
    return Incarnation{publisher, boot, make_incarnation_id(publisher, boot)};
  }
  [[nodiscard]] constexpr bool is_set() const noexcept { return id.is_set(); }

  friend constexpr bool operator==(const Incarnation&, const Incarnation&) noexcept = default;
};

/// The authority under which a single request is evaluated.
struct AuthorityVector {
  DomainId domain{};
  AccountId account{};
  EpochId epoch{};
  Generation generation{};
  IncarnationId incarnation{};

  [[nodiscard]] constexpr bool is_complete() const noexcept {
    return domain.is_set() && account.is_set() && epoch.is_set() && generation.is_set() && incarnation.is_set();
  }

  [[nodiscard]] Digest128 digest() const noexcept {
    return DigestBuilder{}
        .u64(domain.value())
        .u64(account.value())
        .u64(epoch.value())
        .u64(generation.value())
        .u64(incarnation.value())
        .finish();
  }

  friend constexpr bool operator==(const AuthorityVector&, const AuthorityVector&) noexcept = default;
};

/// Durable record of an incarnation's standing with the authority.
struct IncarnationRecord {
  IncarnationId id{};
  PublisherId publisher{};
  BootId boot{};
  AuthorityState state{AuthorityState::Unknown};
  EpochId reconciled_epoch{};
  Generation reconciled_generation{};
  Digest128 reconciliation_proof{};
  // Bound relationship: the first issue from this incarnation fixes which
  // producer/consumer/grant pair it speaks for. A later attempt that claims a
  // different pair is refused rather than silently accepted.
  ProducerId producer{};
  ConsumerId consumer{};
  GrantId grant{};
  std::uint64_t last_sequence{0};
  std::uint64_t committed_ops{0};
  std::uint64_t refused_ops{0};
  std::uint64_t fenced_at_sequence{0};

  [[nodiscard]] bool is_live(const EpochId epoch, const Generation generation) const noexcept {
    return state == AuthorityState::Reconciled && reconciled_epoch == epoch && reconciled_generation == generation;
  }
};

/// Permanent fence over an incarnation. Fences are append-only and are never
/// removed by any operation, including epoch advance and restart.
struct FenceRecord {
  IncarnationId incarnation{};
  EpochId epoch{};
  Generation generation{};
  Reason cause{Reason::UnknownState};
  std::uint64_t sequence{0};
  std::uint64_t fenced_credit{0};
};

/// Proof that a caller participated in THIS authority boot.
///
/// The nonce is regenerated on every boot, so a proof captured before a restart
/// (or before an epoch advance, because the proof binds the authority vector)
/// is worthless. This is a freshness and round-trip proof, not authentication:
/// the transport is unauthenticated loopback and Credit Fabric makes no claim
/// that a local process cannot claim an incarnation identity.
class ReconciliationChallenge {
 public:
  ReconciliationChallenge() noexcept;

  [[nodiscard]] const Digest128& nonce() const noexcept { return nonce_; }

  [[nodiscard]] static Digest128 compute(Digest128 nonce, const Incarnation& incarnation,
                                         const AuthorityVector& authority) noexcept;
  [[nodiscard]] Digest128 prove(const Incarnation& incarnation, const AuthorityVector& authority) const noexcept;
  [[nodiscard]] bool verify(const Incarnation& incarnation, const AuthorityVector& authority,
                            Digest128 proof) const noexcept;

 private:
  Digest128 nonce_{};
};

}  // namespace creditfabric

#endif  // CREDITFABRIC_AUTHORITY_HPP
