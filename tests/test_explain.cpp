// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Explanation is a bounded, deterministic projection. It never grows without
// limit and it never invents authority it does not have.

#include <cstdint>
#include <string>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

namespace {

Incarnation worker(std::uint64_t index) {
  return Incarnation::make(PublisherId{0x3000ull + index}, BootId{0xC0DEull + index});
}

}  // namespace

CF_TEST(an_unknown_account_explains_itself_without_inventing_state) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  const Explanation explanation = fixture.explain();
  CHECK(!explanation.found);
  CHECK(!explanation.view.closed);
  CHECK_EQ(explanation.view.capacity, 0ull);
  CHECK_EQ(explanation.committed_attempts, 0ull);
  const std::string text = render(explanation);
  CHECK(text.find("UNKNOWN") != std::string::npos);
}

CF_TEST(the_explanation_reports_every_governed_quantity) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 400).ok());
  REQUIRE(fixture.call(OpKind::Consume, 100).ok());
  REQUIRE(fixture.call(OpKind::Protect, 50).ok());

  const Explanation explanation = fixture.explain();
  REQUIRE(explanation.found);
  CHECK_EQ(explanation.view.capacity, 1000ull);
  CHECK_EQ(explanation.view.issued, 400ull);
  CHECK_EQ(explanation.view.consumed, 100ull);
  CHECK_EQ(explanation.view.spent, 100ull);
  CHECK_EQ(explanation.view.returned, 0ull);
  CHECK_EQ(explanation.view.in_flight, 400ull);
  CHECK_EQ(explanation.view.outstanding, 300ull);
  CHECK_EQ(explanation.view.protected_credits, 50ull);
  CHECK_EQ(explanation.view.available, 550ull);
  CHECK_EQ(explanation.view.quarantined, 0ull);
  CHECK(explanation.view.closed);
  CHECK(explanation.state_digest.to_hex().size() == 32);
  CHECK_EQ(explanation.operator_incarnation, fixture.me.id);

  const std::string text = render(explanation);
  CHECK(text.find("capacity=1000") != std::string::npos);
  CHECK(text.find("in_flight=400") != std::string::npos);
  CHECK(text.find("state_digest=") != std::string::npos);
  CHECK(text.find("incarnation ") != std::string::npos);
}

CF_TEST(refusals_are_explained_with_their_exact_reason) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100).ok());
  const CreditOutcome refused = fixture.call(OpKind::Return, 5);
  CHECK(!refused.ok());
  CHECK_EQ(refused.status.reason, Reason::InsufficientOutstanding);

  const Explanation explanation = fixture.explain();
  CHECK(explanation.refused_attempts >= 1);
  REQUIRE(!explanation.refusals.empty());
  CHECK_EQ(explanation.refusals.back().reason, Reason::InsufficientOutstanding);
  CHECK_EQ(std::string(to_string(explanation.refusals.back().reason)), std::string("InsufficientOutstanding"));
  const std::string text = render(explanation);
  CHECK(text.find("reason=InsufficientOutstanding") != std::string::npos);
}

CF_TEST(the_explanation_stays_bounded_under_a_large_population) {
  cftest::Fixture fixture;
  fixture.limits.max_explanation_incarnations = 4;
  fixture.limits.max_explanation_refusals = 3;
  fixture.limits.max_explanation_fences = 2;
  fixture.engine = std::make_unique<CreditEngine>(fixture.limits,
                                                  std::make_unique<cftest::BorrowedJournal>(fixture.journal),
                                                  fixture.me);
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100000).ok());

  for (std::uint64_t i = 0; i < 12; ++i) {
    std::uint64_t sequence = 0;
    REQUIRE(fixture.reconcile(worker(i), sequence).ok());
  }
  for (int i = 0; i < 20; ++i) {
    (void)fixture.call(OpKind::Return, 1);  // decided refusals
  }
  for (std::uint64_t i = 0; i < 6; ++i) {
    REQUIRE(fixture.call(OpKind::Fence, 0, 0, worker(i).id, Reason::Cancelled).ok());
  }

  const Explanation explanation = fixture.explain();
  CHECK_EQ(explanation.incarnations_total, 13ull);  // operator plus twelve workers
  CHECK(explanation.incarnations.size() <= 4ull);
  CHECK(explanation.refusals.size() <= 3ull);
  CHECK(explanation.fences.size() <= 2ull);
  CHECK_EQ(explanation.fences_total, 6ull);
  CHECK(explanation.refusals_total >= 10ull);

  // The rendering stays proportional to the bound, not to the population.
  const std::string text = render(explanation);
  CHECK(text.size() < 4096ull);
}

CF_TEST(fenced_and_ambiguous_state_is_visible) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  const Incarnation victim = worker(90);
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(victim, sequence).ok());
  REQUIRE(fixture.call(OpKind::Fence, 0, 0, victim.id, Reason::BudgetExceeded).ok());

  const Explanation explanation = fixture.explain();
  REQUIRE(explanation.fences.size() >= 1ull);
  CHECK_EQ(explanation.fences[0].cause, Reason::BudgetExceeded);
  bool victim_fenced = false;
  for (const IncarnationSummary& summary : explanation.incarnations) {
    if (summary.id == victim.id) {
      victim_fenced = true;
      CHECK_EQ(summary.state, AuthorityState::Fenced);
    }
  }
  CHECK(victim_fenced);
  const std::string text = render(explanation);
  CHECK(text.find("FENCED") != std::string::npos);
  CHECK(text.find("fence ") != std::string::npos);
}

CF_TEST(the_same_state_always_renders_the_same_digest) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(1000).ok());
  REQUIRE(fixture.call(OpKind::Issue, 10).ok());
  const Digest128 first = fixture.explain().state_digest;
  const Digest128 second = fixture.explain().state_digest;
  CHECK(first == second);

  REQUIRE(fixture.call(OpKind::Issue, 1).ok());
  CHECK_NE(fixture.explain().state_digest, first);
}

CF_TEST_MAIN()
