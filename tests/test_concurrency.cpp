// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency: many real threads drive one authority. The engine serializes
// every decision under a single leaf lock, so the only acceptable outcome is
// exact bookkeeping: no lost update, no double spend, no partial mutation.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "support/fixture.hpp"
#include "support/test_framework.hpp"

using namespace creditfabric;

namespace {

struct Worker {
  Incarnation incarnation{};
  std::uint64_t sequence{1};
  AttemptIdGenerator attempts{};
  std::uint64_t granted{0};
  std::uint64_t consumed{0};
  std::uint64_t returned{0};
  std::uint64_t refusals{0};
  std::uint64_t pending{0};
};

}  // namespace

CF_TEST(concurrent_workers_never_lose_or_duplicate_credit) {
  cftest::Fixture fixture;
  fixture.limits.max_attempt_window = 8192;
  fixture.engine = std::make_unique<CreditEngine>(fixture.limits,
                                                  std::make_unique<cftest::BorrowedJournal>(fixture.journal),
                                                  fixture.me);
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100000).ok());

  constexpr std::size_t kWorkers = 8;
  constexpr std::uint64_t kRounds = 400;
  std::vector<Worker> workers(kWorkers);
  for (std::size_t i = 0; i < kWorkers; ++i) {
    workers[i].incarnation = Incarnation::make(PublisherId{0x5000ull + i}, BootId{0xF00Dull + i});
    workers[i].attempts = AttemptIdGenerator{0x9000ull + i};
    std::uint64_t placed = 0;
    REQUIRE(fixture.reconcile(workers[i].incarnation, placed).ok());
    workers[i].sequence = placed;
  }

  std::atomic<std::uint64_t> barrier{0};
  std::vector<std::thread> threads;
  threads.reserve(kWorkers);
  for (std::size_t index = 0; index < kWorkers; ++index) {
    threads.emplace_back([&fixture, &workers, &barrier, index]() {
      Worker& self = workers[index];
      const AuthorityVector authority = fixture.authority_of(self.incarnation);
      barrier.fetch_add(1, std::memory_order_acq_rel);
      while (barrier.load(std::memory_order_acquire) < kWorkers) {
        std::this_thread::yield();
      }
      for (std::uint64_t round = 0; round < kRounds; ++round) {
        const bool issuing = (round % 4 == 0) || self.pending == 0;
        CreditRequest request{};
        request.presenter = self.incarnation;
        request.authority = authority;
        request.sequence = self.sequence;
        request.attempt = self.attempts.next();
        if (issuing) {
          request.op = OpKind::Issue;
          request.count = 1 + (round % 7);
        } else if (round % 4 == 1) {
          request.op = OpKind::Consume;
          request.count = 1;
        } else {
          request.op = OpKind::Return;
          request.count = 1 + (self.pending / 2);
        }
        const CreditOutcome outcome = fixture.engine->apply(request);
        if (!outcome.ok()) {
          ++self.refusals;
          continue;
        }
        if (outcome.decided && !outcome.duplicate) self.sequence = request.sequence + 1;
        if (outcome.duplicate) continue;
        switch (request.op) {
          case OpKind::Issue:
            self.granted += request.count;
            self.pending += request.count;
            break;
          case OpKind::Consume:
            self.consumed += request.count;
            self.pending -= request.count;
            break;
          case OpKind::Return:
            self.returned += request.count;
            self.pending -= request.count;
            break;
          default:
            break;
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  std::uint64_t granted = 0;
  std::uint64_t consumed = 0;
  std::uint64_t returned = 0;
  std::uint64_t refused = 0;
  std::uint64_t pending = 0;
  for (const Worker& worker : workers) {
    granted += worker.granted;
    consumed += worker.consumed;
    returned += worker.returned;
    refused += worker.refusals;
    pending += worker.pending;
  }

  const Explanation explanation = fixture.explain();
  CHECK(explanation.view.closed);
  CHECK_EQ(explanation.view.issued, granted);
  CHECK_EQ(explanation.view.consumed, consumed);
  CHECK_EQ(explanation.view.returned, returned);
  // In-flight credit is everything handed out and not handed back: the
  // spendable remainder plus what has already been spent.
  CHECK_EQ(explanation.view.in_flight, pending + consumed);
  CHECK_EQ(explanation.view.outstanding + explanation.view.spent + explanation.view.stale,
           explanation.view.in_flight);
  CHECK_EQ(explanation.view.available + explanation.view.protected_credits + explanation.view.in_flight,
           explanation.view.capacity);
  CHECK_EQ(explanation.refused_attempts, refused);
  CHECK(granted > 0);
  CHECK(consumed > 0);
}

CF_TEST(concurrent_duplicates_of_one_attempt_mutate_exactly_once) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(10000).ok());

  constexpr std::size_t kThreads = 8;
  std::vector<Incarnation> incarnations;
  std::vector<std::uint64_t> sequences;
  for (std::size_t i = 0; i < kThreads; ++i) {
    const Incarnation worker = Incarnation::make(PublisherId{0x6000ull + i}, BootId{0xBEEFull + i});
    std::uint64_t placed = 0;
    REQUIRE(fixture.reconcile(worker, placed).ok());
    incarnations.push_back(worker);
    sequences.push_back(placed);
  }

  CreditRequest shared{};
  shared.op = OpKind::Issue;
  shared.count = 100;
  shared.presenter = incarnations[0];
  shared.authority = fixture.authority_of(incarnations[0]);
  shared.sequence = sequences[0];
  shared.attempt = AttemptIdGenerator{0x777ull}.next();

  std::vector<CreditOutcome> outcomes(kThreads);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&fixture, &shared, &outcomes, i]() { outcomes[i] = fixture.engine->apply(shared); });
  }
  for (std::thread& thread : threads) thread.join();

  std::size_t applied = 0;
  std::size_t duplicates = 0;
  for (const CreditOutcome& outcome : outcomes) {
    CHECK(outcome.ok());
    if (outcome.applied) ++applied;
    if (outcome.duplicate) ++duplicates;
  }
  CHECK_EQ(applied, 1ull);
  CHECK_EQ(duplicates, kThreads - 1);
  CHECK_EQ(fixture.view().issued, 100ull);
}

CF_TEST(readers_never_observe_a_torn_view) {
  cftest::Fixture fixture;
  REQUIRE(fixture.recover().ok());
  REQUIRE(fixture.open(100000).ok());

  const Incarnation writer = Incarnation::make(PublisherId{0x7000ull}, BootId{0xA11CEull});
  std::uint64_t sequence = 0;
  REQUIRE(fixture.reconcile(writer, sequence).ok());
  const AuthorityVector authority = fixture.authority_of(writer);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observations{0};
  std::atomic<std::uint64_t> broken{0};
  std::thread reader([&fixture, &stop, &observations, &broken]() {
    while (!stop.load(std::memory_order_acquire)) {
      const CreditView view = fixture.explain().view;
      observations.fetch_add(1, std::memory_order_relaxed);
      if (!view.closed || view.available + view.protected_credits + view.in_flight != view.capacity ||
          view.outstanding + view.spent + view.stale != view.in_flight) {
        broken.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  AttemptIdGenerator attempts{0x4242ull};
  for (int round = 0; round < 2000; ++round) {
    CreditRequest request{};
    request.presenter = writer;
    request.authority = authority;
    request.sequence = sequence;
    request.attempt = attempts.next();
    request.op = (round % 3 == 0) ? OpKind::Issue : ((round % 3 == 1) ? OpKind::Consume : OpKind::Return);
    request.count = (round % 3 == 2) ? 2 : 1;
    const CreditOutcome outcome = fixture.engine->apply(request);
    if (outcome.decided) sequence = request.sequence + 1;
  }

  stop.store(true, std::memory_order_release);
  reader.join();
  CHECK_EQ(broken.load(), 0ull);
  CHECK(observations.load() > 0ull);
  CHECK(fixture.view().closed);
}

CF_TEST_MAIN()
