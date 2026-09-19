// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Attempt identity: a decided attempt is decided exactly once. Replaying it
// returns the recorded outcome; contradicting it is refused; replaying
// something older than the window is refused as stale, never re-applied.

#include <cstdint>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

namespace {

CreditOutcome submit(cftest::Fixture& fixture, OpKind op, std::uint64_t count) {
  CreditRequest request{};
  request.op = op;
  request.count = count;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  const CreditOutcome outcome = fixture.engine->apply(request);
  if (outcome.decided) fixture.sequence = request.sequence + 1;
  return outcome;
}

/// Retries the exact same attempt identity.
CreditOutcome replay(const CreditRequest& original, cftest::Fixture& fixture) {
  (void)fixture;
  return fixture.engine->apply(original);
}

}  // namespace

CF_TEST(a_duplicated_issue_is_absorbed) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 250;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;

  const CreditOutcome first = fixture.engine->apply(request);
  REQUIRE(first.ok());
  CHECK(first.applied);
  CHECK(!first.duplicate);
  CHECK_EQ(fixture.view().issued, 250ull);

  const CreditOutcome second = replay(request, fixture);
  CHECK(second.ok());
  CHECK(!second.applied);
  CHECK(second.duplicate);
  CHECK(second.decided);
  CHECK_EQ(second.sequence, 2ull);
  CHECK_EQ(fixture.view().issued, 250ull);
  CHECK_EQ(fixture.view().available, 750ull);

  const CreditOutcome third = replay(request, fixture);
  CHECK(third.duplicate);
  CHECK_EQ(fixture.view().issued, 250ull);
  CHECK_EQ(fixture.engine->stats().duplicates_absorbed, 2ull);
}

CF_TEST(a_duplicated_consume_and_return_are_absorbed) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(submit(fixture, OpKind::Issue, 100).ok());

  CreditRequest consume{};
  consume.op = OpKind::Consume;
  consume.count = 40;
  consume.presenter = fixture.me;
  consume.authority = fixture.authority_of(fixture.me);
  consume.attempt = fixture.attempts.next();
  consume.sequence = fixture.sequence;
  REQUIRE(fixture.engine->apply(consume).ok());
  fixture.sequence = consume.sequence + 1;

  CreditRequest give{};
  give.op = OpKind::Return;
  give.count = 90;
  give.presenter = fixture.me;
  give.authority = fixture.authority_of(fixture.me);
  give.attempt = fixture.attempts.next();
  give.sequence = fixture.sequence;
  REQUIRE(fixture.engine->apply(give).ok());
  fixture.sequence = give.sequence + 1;

  CHECK_EQ(fixture.view().consumed, 40ull);
  CHECK_EQ(fixture.view().returned, 90ull);
  CHECK_EQ(fixture.view().in_flight, 10ull);

  CHECK(fixture.engine->apply(consume).duplicate);
  CHECK(fixture.engine->apply(give).duplicate);
  CHECK_EQ(fixture.view().consumed, 40ull);
  CHECK_EQ(fixture.view().returned, 90ull);
  CHECK_EQ(fixture.view().in_flight, 10ull);
  CHECK(fixture.view().closed);
}

CF_TEST(the_same_identity_with_different_content_is_a_contradiction) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(submit(fixture, OpKind::Issue, 100).ok());

  CreditRequest conflicting{};
  conflicting.op = OpKind::Issue;
  conflicting.count = 101;  // different content, same (incarnation, sequence)
  conflicting.presenter = fixture.me;
  conflicting.authority = fixture.authority_of(fixture.me);
  conflicting.attempt = fixture.attempts.next();
  conflicting.sequence = 2;

  const CreditOutcome outcome = fixture.engine->apply(conflicting);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::AttemptConflict);
  CHECK(outcome.decided);
  CHECK_EQ(fixture.view().issued, 100ull);
}

CF_TEST(a_reused_attempt_id_under_a_new_sequence_is_a_contradiction) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 10;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  REQUIRE(fixture.engine->apply(request).ok());

  request.sequence = fixture.sequence + 1;  // same attempt id, next sequence slot
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::AttemptConflict);
  CHECK_EQ(fixture.view().issued, 10ull);
}

CF_TEST(work_below_the_watermark_is_refused_as_stale) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(submit(fixture, OpKind::Issue, 10).ok());
  REQUIRE(submit(fixture, OpKind::Issue, 10).ok());

  CreditRequest stale{};
  stale.op = OpKind::Issue;
  stale.count = 10;
  stale.presenter = fixture.me;
  stale.authority = fixture.authority_of(fixture.me);
  stale.attempt = fixture.attempts.next();
  stale.sequence = 1;  // long decided, and no longer in the window
  const CreditOutcome outcome = fixture.engine->apply(stale);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::AttemptConflict);  // still in the window
  CHECK_EQ(fixture.view().issued, 20ull);
}

CF_TEST(a_sequence_gap_is_refused_and_reports_the_expected_slot) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(submit(fixture, OpKind::Issue, 10).ok());

  CreditRequest ahead{};
  ahead.op = OpKind::Issue;
  ahead.count = 10;
  ahead.presenter = fixture.me;
  ahead.authority = fixture.authority_of(fixture.me);
  ahead.attempt = fixture.attempts.next();
  ahead.sequence = 5;
  const CreditOutcome outcome = fixture.engine->apply(ahead);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::SequenceGap);
  CHECK_EQ(outcome.status.detail, 3ull);
  CHECK(!outcome.decided);
  CHECK_EQ(fixture.view().issued, 10ull);

  ahead.attempt = fixture.attempts.next();
  ahead.sequence = 3;
  CHECK(fixture.engine->apply(ahead).ok());
}

CF_TEST(a_sequence_of_zero_is_structurally_invalid) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 1;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = 0;
  const CreditOutcome outcome = fixture.engine->apply(request);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::MalformedMessage);
  CHECK(!outcome.decided);
}

CF_TEST(an_evicted_attempt_is_refused_rather_than_re_decided) {
  cftest::Fixture fixture;
  fixture.limits.max_attempt_window = 4;
  fixture.engine = std::make_unique<CreditEngine>(fixture.limits,
                                                  std::make_unique<cftest::BorrowedJournal>(fixture.journal),
                                                  fixture.me);
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest first{};
  first.op = OpKind::Issue;
  first.count = 1;
  first.presenter = fixture.me;
  first.authority = fixture.authority_of(fixture.me);
  first.attempt = fixture.attempts.next();
  first.sequence = fixture.sequence;
  REQUIRE(fixture.engine->apply(first).ok());
  fixture.sequence += 1;

  for (int i = 0; i < 8; ++i) {
    REQUIRE(submit(fixture, OpKind::Issue, 1).ok());
  }
  const std::uint64_t issued_before = fixture.view().issued;

  // The original identity has fallen out of the bounded window. It must be
  // refused as stale replay, never applied a second time.
  const CreditOutcome outcome = fixture.engine->apply(first);
  CHECK(!outcome.ok());
  CHECK_EQ(outcome.status.reason, Reason::StaleReplay);
  CHECK_EQ(outcome.status.detail, fixture.sequence - 1);
  CHECK_EQ(fixture.view().issued, issued_before);
}

CF_TEST(a_duplicate_does_not_advance_the_watermark) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 5;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  REQUIRE(fixture.engine->apply(request).ok());
  REQUIRE(fixture.engine->apply(request).duplicate);

  // The next legitimate attempt still lands on the following slot.
  request.op = OpKind::Issue;
  request.count = 5;
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence + 1;
  CHECK(fixture.engine->apply(request).ok());
  CHECK_EQ(fixture.view().issued, 10ull);
}

CF_TEST(a_refused_decided_attempt_is_replayed_verbatim) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100).ok());

  CreditRequest request{};
  request.op = OpKind::Return;
  request.count = 5;  // nothing is in flight yet
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  const CreditOutcome first = fixture.engine->apply(request);
  CHECK(!first.ok());
  CHECK_EQ(first.status.reason, Reason::InsufficientOutstanding);
  CHECK(first.decided);

  const CreditOutcome second = fixture.engine->apply(request);
  CHECK(!second.ok());
  CHECK_EQ(second.status.reason, Reason::InsufficientOutstanding);
  CHECK(second.duplicate);
  CHECK_EQ(fixture.view().returned, 0ull);
}

CF_TEST(pre_admission_rejections_carry_no_durable_identity) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());

  CreditRequest request{};
  request.op = OpKind::Issue;
  request.count = 10;
  request.presenter = fixture.me;
  request.authority = fixture.authority_of(fixture.me);
  request.authority.epoch = EpochId{99};  // stale authority
  request.attempt = fixture.attempts.next();
  request.sequence = fixture.sequence;
  const CreditOutcome refused_once = fixture.engine->apply(request);
  CHECK_EQ(refused_once.status.reason, Reason::StaleAuthority);
  CHECK(!refused_once.decided);

  // Fixing the authority lets the same sequence succeed: nothing was consumed.
  request.authority = fixture.authority_of(fixture.me);
  CHECK(fixture.engine->apply(request).ok());
  CHECK_EQ(fixture.view().issued, 10ull);
}

CF_TEST_MAIN()
