// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Authority binding: UNKNOWN never authorizes, stale credit never becomes
// spendable again, and a fence is permanent.

#include <cstdint>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

namespace {

Incarnation worker(std::uint64_t index) {
  return Incarnation::make(PublisherId{0x2000ull + index}, BootId{0xB007ull + index});
}

}  // namespace

CF_TEST(an_unreconciled_incarnation_cannot_issue) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  const Incarnation stranger = worker(1);
  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 10;
  request.presenter = stranger;
  request.authority = fixture.authority_of(stranger);
  request.attempt = fixture.attempts.next();
  request.sequence = 1;

  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::UnknownIncarnation);
  CHECK(!outcome.decided);
  CHECK_EQ(fixture.view().issued, 0ull);

  // The same identity is still replayed as a fresh decision, because the
  // pre-admission rejection consumed no durable identity.
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(stranger, sequence).ok());
  request.attempt = fixture.attempts.next();
  request.sequence = sequence;
  CHECK(fixture.engine->apply(request).ok());
}

CF_TEST(reconciliation_requires_the_current_proof) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  const Incarnation stranger = worker(2);
  CreditRequest request{};
  request.op = OpKind::Reconcile;
  request.presenter = stranger;
  request.authority = fixture.authority_of(stranger);
  request.attempt = fixture.attempts.next();
  request.sequence = 1;

  request.proof = Digest128{1, 2};
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::ReconciliationRequired);

  request.attempt = fixture.attempts.next();
  request.proof = fixture.engine->challenge().prove(stranger, request.authority);
  CHECK(fixture.engine->apply(request).ok());

  // Reusing the decided slot with different content is a contradiction.
  request.attempt = fixture.attempts.next();
  request.sequence = 1;
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::AttemptConflict);
}

CF_TEST(epoch_advance_makes_previous_authority_stale) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 400).ok());

  const AuthorityVector stale_authority = fixture.authority_of(fixture.me);
  CreditRequest stale{};
  stale.op = OpKind::Consume;
  stale.count = 10;
  stale.presenter = fixture.me;
  stale.authority = stale_authority;
  stale.attempt = fixture.attempts.next();
  stale.sequence = fixture.sequence;

  REQUIRE(fixture.call(OpKind::AdvanceEpoch).ok());
  const Explanation advanced = fixture.explain();
  CHECK_EQ(advanced.authority.epoch.value(), 2ull);
  CHECK_EQ(advanced.authority.generation.value(), 2ull);
  // Credit that was in flight is quarantined, never silently returned.
  CHECK_EQ(advanced.view.quarantined, 400ull);
  CHECK_EQ(advanced.view.in_flight, 400ull);
  CHECK_EQ(advanced.view.outstanding, 0ull);
  CHECK_EQ(advanced.view.available, 600ull);
  CHECK(advanced.view.closed);

  const CreditOutcome refused = fixture.engine->apply(stale);
  CHECK(!refused.ok());
  CHECK_EQ(refused.status.reason, Reason::StaleAuthority);
  CHECK_EQ(refused.status.detail, 2ull);

  // The stale era's proof no longer reconciles anything.
  CreditRequest stale_proof{};
  stale_proof.op = OpKind::Reconcile;
  stale_proof.presenter = fixture.me;
  stale_proof.authority = stale_authority;
  stale_proof.attempt = fixture.attempts.next();
  stale_proof.sequence = fixture.sequence;
  stale_proof.proof = fixture.engine->challenge().prove(fixture.me, stale_authority);
  CHECK(!fixture.engine->apply(stale_proof).ok());

  // Revalidating quarantined credit as returned puts it back in the pool.
  REQUIRE(fixture.call(OpKind::Revalidate, 400, 0, IncarnationId{}, Reason::PolicyRefused,
                       RevalidationDecision::ToReturned)
              .ok());
  const Explanation revalidated = fixture.explain();
  CHECK_EQ(revalidated.view.quarantined, 0ull);
  CHECK_EQ(revalidated.view.available, 1000ull);
  CHECK_EQ(revalidated.view.in_flight, 0ull);
  CHECK(revalidated.view.closed);
}

CF_TEST(explicit_epoch_rotation_requires_a_strictly_greater_epoch) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Rotate, 500).ok());
  CHECK_EQ(fixture.explain().authority.epoch.value(), 500ull);

  const CreditOutcome backwards = fixture.call(OpKind::Rotate, 499);
  CHECK(!backwards.ok());
  CHECK_EQ(backwards.status.reason, Reason::InvalidState);
  CHECK_EQ(backwards.status.detail, 500ull);
}

CF_TEST(revalidating_quarantined_credit_as_spent_keeps_it_occupied) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 300).ok());
  REQUIRE(fixture.call(OpKind::AdvanceEpoch).ok());
  REQUIRE(fixture.call(OpKind::Revalidate, 300, 0, IncarnationId{}, Reason::PolicyRefused,
                       RevalidationDecision::ToConsumed)
              .ok());
  const Explanation explanation = fixture.explain();
  CHECK_EQ(explanation.view.quarantined, 0ull);
  CHECK_EQ(explanation.view.spent, 300ull);
  CHECK_EQ(explanation.view.in_flight, 300ull);
  CHECK_EQ(explanation.view.available, 700ull);
  CHECK(explanation.view.closed);
}

CF_TEST(fencing_is_permanent) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  const Incarnation victim = worker(3);
  std::uint64_t victim_sequence = 0;
  REQUIRE(fixture.reconcile(victim, victim_sequence).ok());

  CreditRequest issue{};
  issue.op = OpKind::Issue;
  issue.count = 100;
  issue.presenter = victim;
  issue.authority = fixture.authority_of(victim);
  issue.attempt = fixture.attempts.next();
  issue.sequence = victim_sequence;
  REQUIRE(fixture.engine->apply(issue).ok());
  ++victim_sequence;

  REQUIRE(fixture.call(OpKind::Fence, 0, 0, victim.id, Reason::Cancelled).ok());
  const Explanation fenced = fixture.explain();
  CHECK_EQ(fenced.fences_total, 1ull);
  CHECK_EQ(fenced.fences[0].incarnation, victim.id);
  CHECK_EQ(fenced.fences[0].cause, Reason::Cancelled);
  CHECK_EQ(fenced.fences[0].fenced_credit, 100ull);

  // Every later attempt from the fenced incarnation is refused.
  issue.attempt = fixture.attempts.next();
  issue.sequence = victim_sequence;
  const CreditOutcome refused = fixture.engine->apply(issue);
  CHECK(!refused.ok());
  CHECK_EQ(refused.status.reason, Reason::FencedIncarnation);

  // Even reconciliation cannot revive it.
  CreditRequest reconcile{};
  reconcile.op = OpKind::Reconcile;
  reconcile.presenter = victim;
  reconcile.authority = fixture.authority_of(victim);
  reconcile.attempt = fixture.attempts.next();
  reconcile.sequence = victim_sequence;
  reconcile.proof = fixture.engine->challenge().prove(victim, reconcile.authority);
  CHECK_EQ(fixture.engine->apply(reconcile).status.reason, Reason::FencedIncarnation);

  // Fencing again is a decided non-event, not a second fence record.
  CHECK_EQ(fixture.call(OpKind::Fence, 0, 0, victim.id, Reason::Cancelled).status.reason, Reason::InvalidState);
  CHECK_EQ(fixture.explain().fences_total, 1ull);
}

CF_TEST(a_fence_survives_an_epoch_advance) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  const Incarnation victim = worker(4);
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(victim, sequence).ok());
  REQUIRE(fixture.call(OpKind::Fence, 0, 0, victim.id, Reason::Cancelled).ok());
  REQUIRE(fixture.call(OpKind::AdvanceEpoch).ok());

  CreditRequest request{};
  request.op = OpKind::Reconcile;
  request.presenter = victim;
  request.authority = fixture.authority_of(victim);
  request.attempt = fixture.attempts.next();
  request.sequence = sequence;
  request.proof = fixture.engine->challenge().prove(victim, request.authority);
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::FencedIncarnation);
}

CF_TEST(retiring_an_incarnation_closes_it_without_fencing_it) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  const Incarnation other = worker(5);
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(other, sequence).ok());
  REQUIRE(fixture.call(OpKind::RetireIncarnation, 0, 0, other.id).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 1;
  request.presenter = other;
  request.authority = fixture.authority_of(other);
  request.attempt = fixture.attempts.next();
  request.sequence = sequence;
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::IncarnationNotReconciled);
  CHECK_EQ(fixture.explain().fences_total, 0ull);
}

CF_TEST(control_operations_require_the_operator_incarnation) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  const Incarnation other = worker(6);
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(other, sequence).ok());

  CreditRequest request{};
  request.op = OpKind::AdvanceEpoch;
  request.presenter = other;
  request.authority = fixture.authority_of(other);
  request.attempt = fixture.attempts.next();
  request.sequence = sequence;
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::NotOperator);
  CHECK_EQ(fixture.explain().authority.epoch.value(), 1ull);

  // Data operations from the same incarnation are still perfectly legal.
  request.op = OpKind::Issue;
  request.count = 5;
  request.attempt = fixture.attempts.next();
  CHECK(fixture.engine->apply(request).ok());
}

CF_TEST(a_wrong_domain_is_refused) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 1;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.authority.domain = DomainId{0xBAD};
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::DomainMismatch);
}

CF_TEST(stale_evidence_binding_refuses_a_control_decision) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 10).ok());

  CreditRequest request{};
  request.op = OpKind::SetCapacity;
  request.new_capacity = 900;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.expected_state = Digest128{0x1111ull, 0x2222ull};  // evidence for a different state
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::StaleEvidence);
  CHECK_EQ(fixture.view().capacity, 1000ull);

  // Retrying with no binding (or the exact current digest) is accepted, and the
  // refused attempt consumed no sequence number.
  request.expected_state = fixture.explain().state_digest;
  request.attempt = fixture.attempts.next();
  CHECK(fixture.engine->apply(request).ok());
}

CF_TEST_MAIN()
