// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "creditfabric/authority.hpp"

namespace creditfabric {

const char* to_string(AuthorityState state) noexcept {
  switch (state) {
    case AuthorityState::Unknown:
      return "UNKNOWN";
    case AuthorityState::Reconciled:
      return "RECONCILED";
    case AuthorityState::Fenced:
      return "FENCED";
    case AuthorityState::Retired:
      return "RETIRED";
  }
  return "UNKNOWN";
}

ReconciliationChallenge::ReconciliationChallenge() noexcept {
  nonce_ = Digest128{random_u64(), random_u64()};
  if (nonce_.is_zero()) nonce_.lo = 1;
}

Digest128 ReconciliationChallenge::compute(Digest128 nonce, const Incarnation& incarnation,
                                           const AuthorityVector& authority) noexcept {
  return DigestBuilder{Digest128{0x5A5A5A5A5A5A5A5Aull, 0xA5A5A5A5A5A5A5A5ull}}
      .digest(nonce)
      .u64(incarnation.publisher.value())
      .u64(incarnation.boot.value())
      .u64(incarnation.id.value())
      .u64(authority.domain.value())
      .u64(authority.account.value())
      .u64(authority.epoch.value())
      .u64(authority.generation.value())
      .u64(authority.incarnation.value())
      .finish();
}

Digest128 ReconciliationChallenge::prove(const Incarnation& incarnation, const AuthorityVector& authority) const noexcept {
  return compute(nonce_, incarnation, authority);
}

bool ReconciliationChallenge::verify(const Incarnation& incarnation, const AuthorityVector& authority,
                                     Digest128 proof) const noexcept {
  const Digest128 expected = prove(incarnation, authority);
  // Constant-time comparison: no early exit on the first differing byte.
  std::uint64_t difference = (expected.lo ^ proof.lo) | (expected.hi ^ proof.hi);
  return difference == 0;
}

}  // namespace creditfabric
