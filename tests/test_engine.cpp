// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

CF_TEST(open_creates_exactly_one_account) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  const CreditOutcome opened = fixture.open(1000);
  REQUIRE(opened.ok());
  CHECK(opened.applied);
  CHECK(opened.decided);
  CHECK_EQ(fixture.view().capacity, 1000ull);
  CHECK_EQ(fixture.view().available, 1000ull);
  CHECK(fixture.view().closed);

  const CreditOutcome again = fixture.open(1000);
  CHECK(!again.ok());
  CHECK_EQ(again.status.reason, Reason::AccountExists);
  CHECK_EQ(fixture.engine->accounts().size(), 1ull);
}

CF_TEST(open_rejects_structurally_invalid_configuration) {
  {
    cftest::Fixture fixture;
    REQUIRE(fixture.recover().ok());
    CHECK_EQ(fixture.open(0).status.reason, Reason::CapacityOutOfRange);
  }
  {
    cftest::Fixture fixture;
    REQUIRE(fixture.recover().ok());
    CHECK_EQ(fixture.open(100, 50).status.reason, Reason::CapacityOutOfRange);
  }
  {
    cftest::Fixture fixture;
    REQUIRE(fixture.recover().ok());
    CHECK_EQ(fixture.open(100, 100, ProfileKind::PhysicalValidated).status.reason, Reason::ProfileNotSupported);
  }
  {
    cftest::Fixture fixture;
    REQUIRE(fixture.recover().ok());
    // A synthetic profile is a legitimate, non-physical claim.
    CHECK(fixture.open(100, 100, ProfileKind::Synthetic).ok());
  }
}

CF_TEST(open_requires_a_fresh_reconciliation_proof) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  AccountConfig config{};
  config.domain = fixture.domain;
  config.account = fixture.account;
  config.resource = fixture.resource;
  config.capacity = 100;
  config.max_capacity = 100;
  config.rules.max_issue_per_attempt = 100;

  CreditRequest request{};
  request.op = OpKind::Open;
  request.config = config;
  request.presenter = fixture.me;
  request.authority.domain = fixture.domain;
  request.authority.account = fixture.account;
  request.authority.epoch = EpochId{1};
  request.authority.generation = Generation{1};
  request.authority.incarnation = fixture.me.id;
  request.sequence = 1;
  request.attempt = fixture.attempts.next();
  // A proof from a different authority boot is worthless.
  request.proof = Digest128{0xDEADBEEFull, 0xFEEDFACEull};
  CHECK_EQ(fixture.engine->apply(request).status.reason, Reason::ReconciliationRequired);

  request.attempt = fixture.attempts.next();
  request.proof = fixture.engine->challenge().prove(fixture.me, request.authority);
  CHECK(fixture.engine->apply(request).ok());
}

CF_TEST(issue_consume_return_close_the_books_at_every_step) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditOutcome outcome = fixture.call(OpKind::Issue, 400);
  REQUIRE(outcome.ok());
  CHECK_EQ(fixture.view().available, 600ull);
  CHECK_EQ(fixture.view().in_flight, 400ull);
  CHECK_EQ(fixture.view().outstanding, 400ull);
  CHECK_EQ(fixture.view().issued, 400ull);
  CHECK(fixture.view().closed);

  outcome = fixture.call(OpKind::Consume, 150);
  REQUIRE(outcome.ok());
  CHECK_EQ(fixture.view().outstanding, 250ull);
  CHECK_EQ(fixture.view().spent, 150ull);
  CHECK_EQ(fixture.view().consumed, 150ull);
  CHECK_EQ(fixture.view().available, 600ull);
  CHECK(fixture.view().closed);

  outcome = fixture.call(OpKind::Return, 350);
  REQUIRE(outcome.ok());
  CHECK_EQ(fixture.view().in_flight, 50ull);
  CHECK_EQ(fixture.view().spent, 0ull);
  CHECK_EQ(fixture.view().available, 950ull);
  CHECK_EQ(fixture.view().consumed, 150ull);
  CHECK(fixture.view().closed);

  // Return cannot exceed issued/in-flight credit.
  outcome = fixture.call(OpKind::Return, 51);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::InsufficientOutstanding);
  CHECK_EQ(outcome.status.detail, 50ull);
  CHECK(outcome.decided);

  outcome = fixture.call(OpKind::Return, 50);
  REQUIRE(outcome.ok());
  CHECK_EQ(fixture.view().in_flight, 0ull);
  CHECK_EQ(fixture.view().available, 1000ull);
  CHECK(fixture.view().closed);
}

CF_TEST(exhaustion_is_a_latch_not_a_guess) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100).ok());

  CHECK_EQ(fixture.call(OpKind::ClearExhaustion).status.reason, Reason::NotExhausted);

  REQUIRE(fixture.call(OpKind::Exhaust, 0, 0, IncarnationId{}, Reason::BudgetExceeded).ok());
  const Explanation latched = fixture.explain();
  CHECK(latched.hard_exhausted);
  CHECK_EQ(latched.exhaustion_cause, Reason::BudgetExceeded);

  // Hard exhaustion refuses issuance even though capacity is available.
  const CreditOutcome refused_issue = fixture.call(OpKind::Issue, 10);
  CHECK(!refused_issue.ok());
  CHECK_EQ(refused_issue.status.reason, Reason::ExhaustedHard);
  CHECK_EQ(refused_issue.status.detail, static_cast<std::uint64_t>(Reason::BudgetExceeded));
  CHECK_EQ(fixture.view().available, 100ull);

  // Exhausting twice is a decided refusal, not a second latch.
  CHECK_EQ(fixture.call(OpKind::Exhaust, 0, 0, IncarnationId{}, Reason::BudgetExceeded).status.reason,
           Reason::ExhaustedHard);

  REQUIRE(fixture.call(OpKind::ClearExhaustion).ok());
  CHECK(!fixture.explain().hard_exhausted);
  CHECK(fixture.call(OpKind::Issue, 10).ok());
}

CF_TEST(soft_exhaustion_reports_zero_available_without_latching) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(20).ok());
  REQUIRE(fixture.call(OpKind::Issue, 20).ok());
  CHECK(fixture.view().exhausted());
  CHECK_EQ(fixture.view().available, 0ull);
  CHECK(!fixture.explain().hard_exhausted);

  const CreditOutcome outcome = fixture.call(OpKind::Issue, 1);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::InsufficientAvailable);

  REQUIRE(fixture.call(OpKind::Return, 20).ok());
  CHECK(!fixture.view().exhausted());
  CHECK(fixture.view().closed);
}

CF_TEST(protection_withholds_credit_from_issuance) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100).ok());
  REQUIRE(fixture.call(OpKind::Protect, 30).ok());
  CHECK_EQ(fixture.view().available, 70ull);
  CHECK_EQ(fixture.view().protected_credits, 30ull);
  CHECK_EQ(fixture.call(OpKind::Issue, 71).status.reason, Reason::InsufficientAvailable);
  REQUIRE(fixture.call(OpKind::Issue, 70).ok());
  CHECK_EQ(fixture.view().available, 0ull);
  REQUIRE(fixture.call(OpKind::Unprotect, 30).ok());
  CHECK_EQ(fixture.view().available, 30ull);
  CHECK(fixture.view().closed);
}

CF_TEST(capacity_changes_respect_committed_obligations) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100, 200).ok());
  REQUIRE(fixture.call(OpKind::Issue, 40).ok());
  REQUIRE(fixture.call(OpKind::Protect, 10).ok());

  const CreditOutcome too_small = fixture.call(OpKind::SetCapacity, 0, 49);
  CHECK(!too_small.ok());
  CHECK_EQ(too_small.status.reason, Reason::CapacityShrinkViolation);
  CHECK_EQ(too_small.status.detail, 50ull);

  REQUIRE(fixture.call(OpKind::SetCapacity, 0, 50).ok());
  CHECK_EQ(fixture.view().capacity, 50ull);
  CHECK_EQ(fixture.view().available, 0ull);
  CHECK(fixture.view().closed);

  // Growth is refused unless the account was opened with growth enabled.
  CHECK_EQ(fixture.call(OpKind::SetCapacity, 0, 60).status.reason, Reason::PolicyRefused);
  CHECK_EQ(fixture.call(OpKind::SetCapacity, 0, 300).status.reason, Reason::CapacityExceeded);
}

CF_TEST(describe_reports_authority_and_sequence_watermark) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100).ok());
  REQUIRE(fixture.call(OpKind::Issue, 10).ok());

  const CreditOutcome described = fixture.describe(fixture.me);
  REQUIRE(described.ok());
  CHECK_EQ(described.authority.epoch.value(), 1ull);
  CHECK_EQ(described.authority.generation.value(), 1ull);
  CHECK_EQ(described.sequence, fixture.sequence - 1);
  CHECK_EQ(described.view.issued, 10ull);

  // An unknown incarnation may still read the account, but learns that it owns
  // no sequence yet: the watermark it is told is zero.
  const CreditOutcome stranger = fixture.describe(Incarnation::make(PublisherId{9}, BootId{9}));
  CHECK(stranger.ok());
  CHECK_EQ(stranger.sequence, 0ull);
}

CF_TEST(producer_consumer_grant_bind_to_the_issuing_incarnation) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 10;
  request.producer = ProducerId{0xA1ull};
  request.consumer = ConsumerId{7};
  request.grant = GrantId{9};
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  CreditOutcome outcome = fixture.engine->apply(request);
  REQUIRE(outcome.ok());
  fixture.sequence = request.sequence + 1;

  // The same producer/grant pair is accepted again.
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  outcome = fixture.engine->apply(request);
  REQUIRE(outcome.ok());
  fixture.sequence = request.sequence + 1;

  // A different grant id is a contradiction and is refused.
  request.grant = GrantId{10};
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::GrantMismatch);
  CHECK_EQ(outcome.status.detail, 9ull);

  request.grant = GrantId{9};
  request.consumer = ConsumerId{8};
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence + 1;
  outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::ConsumerMismatch);
}

CF_TEST(operations_on_an_unknown_account_are_refused) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  CHECK_EQ(fixture.call(OpKind::Issue, 1).status.reason, Reason::AccountUnknown);
  CHECK(!fixture.explain().found);
}

CF_TEST(engine_refuses_work_before_recovery) {
  cftest::Fixture fixture;
  const CreditOutcome outcome = fixture.open(100);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::ReconciliationRequired);
}

CF_TEST_MAIN()
