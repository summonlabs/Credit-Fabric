// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The accounting core, exercised directly. These tests are the arithmetic
// definition of "total credit accounting closes exactly".

#include <cstdint>
#include <limits>

#include "support/test_framework.hpp"

#include "creditfabric/ledger.hpp"

using namespace creditfabric;

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

bool closes(std::uint64_t capacity, const CreditTotals& totals) {
  const CreditView view = compute_view(capacity, totals);
  if (!view.closed) return false;
  return view.available + view.protected_credits + view.in_flight == capacity &&
         view.outstanding + view.spent + view.stale == view.in_flight;
}

}  // namespace

CF_TEST(credit_view_closes_for_a_fresh_account) {
  const CreditView view = compute_view(100, CreditTotals{});
  CHECK(view.closed);
  CHECK_EQ(view.available, 100ull);
  CHECK_EQ(view.in_flight, 0ull);
  CHECK_EQ(view.outstanding, 0ull);
  CHECK(view.exhausted() == false);
}

CF_TEST(issue_consume_return_recycles_capacity) {
  LedgerPlan plan(100, CreditTotals{});
  REQUIRE(plan.issue(60, 1, 1000));
  CHECK_EQ(plan.view().available, 40ull);
  CHECK_EQ(plan.view().outstanding, 60ull);
  CHECK_EQ(plan.view().in_flight, 60ull);

  REQUIRE(plan.consume(25));
  CHECK_EQ(plan.view().outstanding, 35ull);
  CHECK_EQ(plan.view().spent, 25ull);
  CHECK_EQ(plan.view().consumed, 25ull);
  CHECK_EQ(plan.view().in_flight, 60ull);
  CHECK_EQ(plan.view().available, 40ull);

  // Returning hands capacity back, taking spent credit first.
  REQUIRE(plan.give_back(30));
  CHECK_EQ(plan.view().spent, 0ull);
  CHECK_EQ(plan.view().outstanding, 30ull);
  CHECK_EQ(plan.view().in_flight, 30ull);
  CHECK_EQ(plan.view().available, 70ull);
  CHECK_EQ(plan.view().consumed, 25ull);  // cumulative spend events survive returns
  CHECK(closes(100, plan.totals()));

  // The recycled credit can be issued again: no credit was destroyed.
  REQUIRE(plan.issue(70, 1, 1000));
  CHECK_EQ(plan.view().available, 0ull);
  CHECK(plan.view().exhausted());
  CHECK(closes(100, plan.totals()));
}

CF_TEST(return_cannot_exceed_in_flight_credit) {
  LedgerPlan plan(100, CreditTotals{});
  REQUIRE(plan.issue(40, 1, 100));
  REQUIRE(plan.consume(10));
  const CreditView before = plan.view();
  CHECK_EQ(before.in_flight, 40ull);

  CreditTotals after_failure = plan.totals();
  CHECK(!plan.give_back(41));
  CHECK_EQ(plan.status().reason, Reason::InsufficientOutstanding);
  CHECK_EQ(plan.status().detail, 40ull);
  // A rejected mutation changes nothing, and poisons the plan: the caller must
  // re-plan from committed totals rather than continue a half-decided plan.
  CHECK(plan.totals() == after_failure);
  CHECK(plan.view().in_flight == 40ull);
  CHECK(plan.view().spent == 10ull);
  CHECK(!plan.valid());

  plan = LedgerPlan(100, after_failure);
  REQUIRE(plan.give_back(40));
  CHECK_EQ(plan.view().in_flight, 0ull);
  CHECK_EQ(plan.view().available, 100ull);
  CHECK(closes(100, plan.totals()));

  CHECK(!plan.give_back(1));
  CHECK_EQ(plan.status().reason, Reason::InsufficientOutstanding);
}

CF_TEST(no_credit_can_be_spent_twice) {
  LedgerPlan plan(10, CreditTotals{});
  REQUIRE(plan.issue(4, 1, 10));
  REQUIRE(plan.consume(4));
  CHECK_EQ(plan.view().outstanding, 0ull);
  // The same four credits cannot be spent again while they are in flight.
  CHECK(!plan.consume(1));
  CHECK_EQ(plan.status().reason, Reason::InsufficientOutstanding);
  CHECK_EQ(plan.view().consumed, 4ull);

  // Returning them to the pool does not make them spendable by the same holder.
  LedgerPlan returning(10, plan.totals());
  REQUIRE(returning.give_back(4));
  CHECK_EQ(returning.view().outstanding, 0ull);
  LedgerPlan spending(10, returning.totals());
  CHECK(!spending.consume(4));
  CHECK_EQ(spending.status().reason, Reason::InsufficientOutstanding);

  // Only a fresh issue can authorize spending again.
  LedgerPlan reissued(10, returning.totals());
  REQUIRE(reissued.issue(4, 1, 10));
  REQUIRE(reissued.consume(4));
  CHECK_EQ(reissued.view().consumed, 8ull);
  CHECK(closes(10, reissued.totals()));
}

CF_TEST(issuance_is_bounded_by_available_and_policy) {
  const auto refuses = [](std::uint64_t count, std::uint64_t min_issue, std::uint64_t max_issue) {
    LedgerPlan plan(10, CreditTotals{});
    const bool accepted = plan.issue(count, min_issue, max_issue);
    return accepted ? Reason::Ok : plan.status().reason;
  };
  const auto detail = [](std::uint64_t count, std::uint64_t min_issue, std::uint64_t max_issue) {
    LedgerPlan plan(10, CreditTotals{});
    (void)plan.issue(count, min_issue, max_issue);
    return plan.status().detail;
  };

  CHECK_EQ(refuses(0, 1, 10), Reason::CountZero);
  CHECK_EQ(refuses(3, 4, 10), Reason::PolicyRefused);
  CHECK_EQ(refuses(11, 1, 10), Reason::PolicyRefused);
  CHECK_EQ(refuses(11, 1, 100), Reason::InsufficientAvailable);
  CHECK_EQ(detail(11, 1, 100), 10ull);

  LedgerPlan plan(10, CreditTotals{});
  REQUIRE(plan.issue(10, 1, 10));
  CHECK_EQ(plan.view().available, 0ull);
  CHECK(!plan.issue(1, 1, 10));
  CHECK_EQ(plan.status().reason, Reason::InsufficientAvailable);
}

CF_TEST(protected_credit_is_withheld_from_issue) {
  LedgerPlan plan(10, CreditTotals{});
  REQUIRE(plan.protect(4, true));
  CHECK_EQ(plan.view().available, 6ull);
  CHECK_EQ(plan.view().protected_credits, 4ull);
  CHECK(closes(10, plan.totals()));

  LedgerPlan over = LedgerPlan(10, plan.totals());
  CHECK(!over.issue(7, 1, 100));
  CHECK_EQ(over.status().reason, Reason::InsufficientAvailable);
  CHECK_EQ(over.status().detail, 6ull);

  REQUIRE(plan.issue(6, 1, 100));
  CHECK_EQ(plan.view().available, 0ull);
  CHECK_EQ(plan.view().outstanding, 6ull);

  LedgerPlan no_room(10, plan.totals());
  CHECK(!no_room.protect(1, true));
  CHECK_EQ(no_room.status().reason, Reason::InsufficientAvailable);

  LedgerPlan forbidden(10, plan.totals());
  CHECK(!forbidden.protect(1, false));
  CHECK_EQ(forbidden.status().reason, Reason::PolicyRefused);

  LedgerPlan over_release(10, plan.totals());
  CHECK(!over_release.unprotect(5));
  CHECK_EQ(over_release.status().reason, Reason::InsufficientProtected);
  CHECK_EQ(over_release.view().protected_credits, 4ull);

  LedgerPlan released(10, plan.totals());
  REQUIRE(released.unprotect(4));
  CHECK_EQ(released.view().protected_credits, 0ull);
  CHECK_EQ(released.view().available, 4ull);
  CHECK(closes(10, released.totals()));
}

CF_TEST(quarantine_removes_credit_from_circulation_without_returning_it) {
  LedgerPlan plan(100, CreditTotals{});
  REQUIRE(plan.issue(50, 1, 100));
  REQUIRE(plan.consume(20));
  REQUIRE(plan.quarantine_in_flight());
  CHECK_EQ(plan.view().stale, 50ull);
  CHECK_EQ(plan.view().spent, 0ull);
  CHECK_EQ(plan.view().outstanding, 0ull);
  CHECK_EQ(plan.view().in_flight, 50ull);
  CHECK_EQ(plan.view().available, 50ull);
  CHECK(closes(100, plan.totals()));

  // Quarantined credit is not returnable until it has been revalidated.
  LedgerPlan attempt(100, plan.totals());
  CHECK(!attempt.give_back(1));
  CHECK_EQ(attempt.status().reason, Reason::InsufficientOutstanding);
  CHECK_EQ(attempt.status().detail, 0ull);

  REQUIRE(plan.revalidate_to_returned(30));
  CHECK_EQ(plan.view().stale, 20ull);
  CHECK_EQ(plan.view().in_flight, 20ull);
  CHECK_EQ(plan.view().available, 80ull);
  CHECK(closes(100, plan.totals()));

  REQUIRE(plan.revalidate_to_consumed(20));
  CHECK_EQ(plan.view().stale, 0ull);
  CHECK_EQ(plan.view().spent, 20ull);
  CHECK_EQ(plan.view().in_flight, 20ull);
  CHECK_EQ(plan.view().available, 80ull);
  CHECK(closes(100, plan.totals()));

  CHECK(!plan.revalidate_to_returned(1));
  CHECK_EQ(plan.status().reason, Reason::NothingToRetire);
}

CF_TEST(quarantine_is_idempotent_when_nothing_is_in_flight) {
  LedgerPlan plan(10, CreditTotals{});
  REQUIRE(plan.quarantine_in_flight());
  CHECK_EQ(plan.view().stale, 0ull);
  CHECK(closes(10, plan.totals()));
}

CF_TEST(capacity_shrink_respects_the_obligation_floor) {
  LedgerPlan plan(100, CreditTotals{});
  REQUIRE(plan.protect(10, true));
  REQUIRE(plan.issue(40, 1, 100));

  LedgerPlan too_small(100, plan.totals());
  CHECK(!too_small.set_capacity(49, 100, true));
  CHECK_EQ(too_small.status().reason, Reason::CapacityShrinkViolation);
  CHECK_EQ(too_small.status().detail, 50ull);  // protected + in_flight
  CHECK_EQ(too_small.capacity(), 100ull);

  REQUIRE(plan.set_capacity(50, 100, true));
  CHECK_EQ(plan.capacity(), 50ull);
  CHECK_EQ(plan.view().available, 0ull);
  CHECK(closes(50, plan.totals()));

  LedgerPlan zero(100, plan.totals());
  CHECK(!zero.set_capacity(0, 100, true));
  CHECK_EQ(zero.status().reason, Reason::CapacityOutOfRange);

  LedgerPlan beyond_max(100, plan.totals());
  CHECK(!beyond_max.set_capacity(101, 100, true));
  CHECK_EQ(beyond_max.status().reason, Reason::CapacityExceeded);

  // Growth is refused unless the account policy allows it.
  LedgerPlan no_growth(50, plan.totals());
  CHECK(!no_growth.set_capacity(80, 100, false));
  CHECK_EQ(no_growth.status().reason, Reason::PolicyRefused);
  CHECK_EQ(no_growth.capacity(), 50ull);

  LedgerPlan grown(50, plan.totals());
  REQUIRE(grown.set_capacity(80, 100, true));
  CHECK_EQ(grown.capacity(), 80ull);
  CHECK_EQ(grown.view().available, 30ull);
  CHECK(closes(80, grown.totals()));
  CHECK_EQ(plan.capacity(), 50ull);  // the original plan is untouched
}

CF_TEST(huge_counts_never_wrap) {
  LedgerPlan plan(kMax, CreditTotals{});
  REQUIRE(plan.issue(kMax - 1ull, 1, kMax));
  CHECK_EQ(plan.view().available, 1ull);
  CHECK_EQ(plan.view().outstanding, kMax - 1ull);
  CHECK(closes(kMax, plan.totals()));

  // One more credit fits, a second does not exist.
  REQUIRE(plan.issue(1, 1, kMax));
  CHECK_EQ(plan.view().available, 0ull);

  LedgerPlan over(plan.capacity(), plan.totals());
  CHECK(!over.issue(1, 1, kMax));
  CHECK_EQ(over.status().reason, Reason::InsufficientAvailable);

  REQUIRE(plan.consume(kMax));
  CHECK_EQ(plan.view().consumed, kMax);
  CHECK_EQ(plan.view().outstanding, 0ull);
  REQUIRE(plan.give_back(kMax));
  CHECK_EQ(plan.view().in_flight, 0ull);
  CHECK_EQ(plan.view().available, kMax);
  CHECK(closes(kMax, plan.totals()));
}

CF_TEST(invariants_reject_an_impossible_counter_set) {
  CreditTotals broken{};
  broken.issued = 10;
  broken.returned = 11;
  CHECK(!check_invariants(100, broken).ok());

  CreditTotals over_spent{};
  over_spent.issued = 10;
  over_spent.spent = 11;
  CHECK(!check_invariants(100, over_spent).ok());
  CHECK(!compute_view(100, over_spent).closed);

  CreditTotals over_committed{};
  over_committed.issued = 10;
  over_committed.protected_credits = 95;
  CHECK(!check_invariants(100, over_committed).ok());

  // A broken view saturates instead of reporting nonsense.
  const CreditView view = compute_view(100, over_spent);
  CHECK_EQ(view.available, 0ull);
  CHECK_EQ(view.outstanding, 0ull);
  CHECK(!view.exhausted() || view.available == 0);
}

CF_TEST(a_poisoned_plan_refuses_every_later_operation) {
  LedgerPlan plan(10, CreditTotals{});
  CHECK(!plan.issue(11, 1, 100));
  CHECK(!plan.valid());
  const CreditTotals frozen = plan.totals();
  CHECK(!plan.give_back(1));
  CHECK(!plan.set_capacity(5, 10, true));
  CHECK_EQ(plan.capacity(), 10ull);
  CHECK(plan.totals() == frozen);
}

CF_TEST_MAIN()
