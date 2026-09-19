// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Seeded, property-based closure: a reference model of the accounting rules is
// driven alongside the real authority, and every decision must agree with the
// model, every refusal must be the model's refusal, and both closure identities
// must hold after every single operation.

#include <cstdint>
#include <string>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

namespace {

/// Independent restatement of the published accounting rules.
struct Model {
  std::uint64_t capacity{0};
  std::uint64_t max_capacity{0};
  std::uint64_t issued{0};
  std::uint64_t consumed{0};
  std::uint64_t returned{0};
  std::uint64_t spent{0};
  std::uint64_t stale{0};
  std::uint64_t protected_credits{0};
  std::uint64_t max_issue{0};
  bool hard_exhausted{false};
  bool allow_growth{false};
  bool allow_protect{true};

  [[nodiscard]] std::uint64_t in_flight() const { return issued - returned; }
  [[nodiscard]] std::uint64_t outstanding() const { return in_flight() - spent - stale; }
  [[nodiscard]] std::uint64_t available() const { return capacity - protected_credits - in_flight(); }
  [[nodiscard]] bool closed() const {
    return available() + protected_credits + in_flight() == capacity &&
           outstanding() + spent + stale == in_flight();
  }
};

struct Expectation {
  bool accepted{false};
  Reason reason{Reason::Ok};
};

Expectation expect_issue(const Model& model, std::uint64_t count) {
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.max_issue) return {false, Reason::PolicyRefused};
  if (model.hard_exhausted) return {false, Reason::ExhaustedHard};
  if (count > model.available()) return {false, Reason::InsufficientAvailable};
  return {true, Reason::Ok};
}

Expectation expect_consume(const Model& model, std::uint64_t count) {
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.outstanding()) return {false, Reason::InsufficientOutstanding};
  return {true, Reason::Ok};
}

Expectation expect_return(const Model& model, std::uint64_t count) {
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.in_flight() - model.stale) return {false, Reason::InsufficientOutstanding};
  return {true, Reason::Ok};
}

Expectation expect_protect(const Model& model, std::uint64_t count) {
  if (!model.allow_protect) return {false, Reason::PolicyRefused};
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.available()) return {false, Reason::InsufficientAvailable};
  return {true, Reason::Ok};
}

Expectation expect_unprotect(const Model& model, std::uint64_t count) {
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.protected_credits) return {false, Reason::InsufficientProtected};
  return {true, Reason::Ok};
}

Expectation expect_capacity(const Model& model, std::uint64_t target) {
  if (target == 0) return {false, Reason::CapacityOutOfRange};
  if (target > model.max_capacity) return {false, Reason::CapacityExceeded};
  if (target > model.capacity && !model.allow_growth) return {false, Reason::PolicyRefused};
  if (target < model.protected_credits + model.in_flight()) return {false, Reason::CapacityShrinkViolation};
  return {true, Reason::Ok};
}

Expectation expect_revalidate(const Model& model, std::uint64_t count) {
  if (count == 0) return {false, Reason::CountZero};
  if (count > model.stale) return {false, Reason::NothingToRetire};
  return {true, Reason::Ok};
}

void check_view(const Model& model, const CreditView& view, const char* context) {
  (void)context;
  CHECK(view.closed);
  CHECK_EQ(view.capacity, model.capacity);
  CHECK_EQ(view.issued, model.issued);
  CHECK_EQ(view.consumed, model.consumed);
  CHECK_EQ(view.returned, model.returned);
  CHECK_EQ(view.spent, model.spent);
  CHECK_EQ(view.stale, model.stale);
  CHECK_EQ(view.protected_credits, model.protected_credits);
  CHECK_EQ(view.in_flight, model.in_flight());
  CHECK_EQ(view.outstanding, model.outstanding());
  CHECK_EQ(view.available, model.available());
}

Reason apply_to_model(Model& model, OpKind op, std::uint64_t count, std::uint64_t target,
                      RevalidationDecision decision) {
  switch (op) {
    case OpKind::Issue: {
      const Expectation expectation = expect_issue(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.issued += count;
      return Reason::Ok;
    }
    case OpKind::Consume: {
      const Expectation expectation = expect_consume(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.spent += count;
      model.consumed += count;
      return Reason::Ok;
    }
    case OpKind::Return: {
      const Expectation expectation = expect_return(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.spent -= (model.spent < count) ? model.spent : count;
      model.returned += count;
      return Reason::Ok;
    }
    case OpKind::Protect: {
      const Expectation expectation = expect_protect(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.protected_credits += count;
      return Reason::Ok;
    }
    case OpKind::Unprotect: {
      const Expectation expectation = expect_unprotect(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.protected_credits -= count;
      return Reason::Ok;
    }
    case OpKind::SetCapacity: {
      const Expectation expectation = expect_capacity(model, target);
      if (!expectation.accepted) return expectation.reason;
      model.capacity = target;
      return Reason::Ok;
    }
    case OpKind::AdvanceEpoch: {
      const std::uint64_t quarantined = model.in_flight() - model.stale;
      model.stale += quarantined;
      model.spent = 0;
      return Reason::Ok;
    }
    case OpKind::Revalidate: {
      const Expectation expectation = expect_revalidate(model, count);
      if (!expectation.accepted) return expectation.reason;
      model.stale -= count;
      if (decision == RevalidationDecision::ToReturned) {
        model.returned += count;
      } else {
        model.spent += count;
        model.consumed += count;
      }
      return Reason::Ok;
    }
    case OpKind::Exhaust: {
      if (model.hard_exhausted) return Reason::ExhaustedHard;
      model.hard_exhausted = true;
      return Reason::Ok;
    }
    case OpKind::ClearExhaustion: {
      if (!model.hard_exhausted) return Reason::NotExhausted;
      model.hard_exhausted = false;
      return Reason::Ok;
    }
    default:
      return Reason::Unsupported;
  }
}

void run_sequence(std::uint64_t seed, int operations) {
  cftest::Fixture fixture;
  fixture.limits.max_attempt_window = 4096;
  fixture.engine = std::make_unique<CreditEngine>(fixture.limits,
                                                  std::make_unique<cftest::BorrowedJournal>(fixture.journal),
                                                  fixture.me);
  REQUIRE(fixture.recover().ok());

  const std::uint64_t capacity = 64 + (seed % 512);
  const std::uint64_t max_capacity = capacity * 2;
  REQUIRE(fixture.open(capacity, max_capacity, ProfileKind::Abstract, capacity).ok());

  Model model{};
  model.capacity = capacity;
  model.max_capacity = max_capacity;
  model.max_issue = capacity;
  model.allow_growth = false;
  model.allow_protect = true;
  check_view(model, fixture.view(), "after open");

  cftest::Rng rng{seed};
  for (int step = 0; step < operations; ++step) {
    const std::uint64_t draw = rng.next();
    OpKind op = OpKind::Issue;
    std::uint64_t count = 0;
    std::uint64_t target = 0;
    RevalidationDecision decision = RevalidationDecision::None;

    switch (draw % 10ull) {
      case 0:
      case 1:
        op = OpKind::Issue;
        count = rng.bounded(capacity / 4 + 2);
        break;
      case 2:
      case 3:
        op = OpKind::Consume;
        count = rng.bounded(8);
        break;
      case 4:
      case 5:
        op = OpKind::Return;
        count = rng.bounded(capacity / 2 + 1);
        break;
      case 6:
        op = OpKind::Protect;
        count = rng.bounded(16);
        break;
      case 7:
        op = OpKind::Unprotect;
        count = rng.bounded(16);
        break;
      case 8:
        op = OpKind::SetCapacity;
        target = rng.bounded(max_capacity + 8);
        break;
      case 9:
        switch (rng.bounded(4)) {
          case 0:
            op = OpKind::AdvanceEpoch;
            break;
          case 1:
            op = OpKind::Revalidate;
            count = rng.bounded(16);
            decision = (rng.bounded(2) == 0) ? RevalidationDecision::ToReturned
                                             : RevalidationDecision::ToConsumed;
            break;
          case 2:
            op = OpKind::Exhaust;
            break;
          default:
            op = OpKind::ClearExhaustion;
            break;
        }
        break;
      default:
        break;
    }

    if (op == OpKind::AdvanceEpoch) {
      // Rotation invalidates the operator's authority; the model's ledger rules
      // are independent of that and the fixture revalidates automatically.
      target = 0;
    }

    const Reason expected = apply_to_model(model, op, count, target, decision);
    const CreditOutcome outcome = fixture.call(op, count, target, IncarnationId{}, Reason::BudgetExceeded, decision);

    if (expected == Reason::Ok) {
      if (!outcome.ok()) {
        ::cftest::report_failure(__FILE__, __LINE__,
                                 std::string("step ") + std::to_string(step) + " seed " + std::to_string(seed) +
                                     ": " + std::string(to_string(op)) + " expected acceptance but got " +
                                     std::string(to_string(outcome.status.reason)));
        return;
      }
    } else {
      if (outcome.ok()) {
        ::cftest::report_failure(__FILE__, __LINE__,
                                 std::string("step ") + std::to_string(step) + " seed " + std::to_string(seed) +
                                     ": " + std::string(to_string(op)) + " expected refusal " +
                                     std::string(to_string(expected)) + " but was accepted");
        return;
      }
      if (outcome.status.reason != expected) {
        ::cftest::report_failure(__FILE__, __LINE__,
                                 std::string("step ") + std::to_string(step) + " seed " + std::to_string(seed) +
                                     ": " + std::string(to_string(op)) + " expected " +
                                     std::string(to_string(expected)) + " but got " +
                                     std::string(to_string(outcome.status.reason)));
        return;
      }
      // A refused mutation must not have changed anything.
      check_view(model, fixture.view(), "after refusal");
      continue;
    }
    check_view(model, fixture.view(), "after acceptance");
  }
  CHECK(model.closed());
}

}  // namespace

CF_TEST(the_model_and_the_authority_agree_over_seeded_sequences) {
  const std::uint64_t seeds[] = {1ull, 2ull, 3ull, 0xDEADBEEFull, 0xC0FFEEull, 0x1234567ull, 0xABCDEFull, 0x99ull};
  for (std::uint64_t seed : seeds) {
    run_sequence(seed, 600);
  }
}

CF_TEST(a_long_sequence_still_closes_exactly) {
  run_sequence(0x5EEDF00Dull, 4000);
}

CF_TEST_MAIN()
