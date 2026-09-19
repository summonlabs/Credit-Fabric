// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The benchmark harness is itself tested: it must measure completed work, it
// must be labelled SYNTHETIC, and it must leave the ledger closed.

#include <cstdint>
#include <string>

#include "support/scratch.hpp"
#include "support/test_framework.hpp"

#include "creditfabric/bench.hpp"
#include "creditfabric/platform.hpp"

using namespace creditfabric;

CF_TEST(the_synthetic_benchmark_closes_the_ledger) {
  BenchConfig config{};
  config.operations = 20000;
  config.capacity = 1ull << 20;
  config.producers = 4;
  config.consumers = 4;
  const BenchResult result = run_synthetic_ledger_benchmark(config);

  CHECK_EQ(result.provenance, std::string("SYNTHETIC"));
  CHECK(result.closure_ok);
  CHECK(result.final_view.closed);
  CHECK_EQ(result.final_view.in_flight, 0ull);
  CHECK_EQ(result.final_view.available, result.final_view.capacity);
  CHECK_EQ(result.final_view.spent, 0ull);
  CHECK_EQ(result.final_view.stale, 0ull);
  CHECK_EQ(result.final_view.protected_credits, 0ull);
  CHECK_EQ(result.final_view.issued, result.final_view.returned);
  CHECK_EQ(result.operations_completed, result.operations_attempted);
  CHECK(result.issued > 0);
  CHECK(result.consumed > 0);
  CHECK(result.ops_per_second > 0.0);

  const std::string text = render(result);
  CHECK(text.find("SYNTHETIC") != std::string::npos);
  CHECK(text.find("completed_ops_per_second") != std::string::npos);
  CHECK(text.find("closure=closed") != std::string::npos);
  // The report must not claim anything physical.
  CHECK(text.find("physical") == std::string::npos);
  CHECK(text.find("NIC") == std::string::npos);
}

CF_TEST(the_benchmark_is_deterministic_for_a_fixed_seed) {
  BenchConfig config{};
  config.operations = 4000;
  config.capacity = 1ull << 16;
  config.seed = 0x5EEDull;
  const BenchResult first = run_synthetic_ledger_benchmark(config);
  const BenchResult second = run_synthetic_ledger_benchmark(config);
  CHECK_EQ(first.issued, second.issued);
  CHECK_EQ(first.consumed, second.consumed);
  CHECK_EQ(first.returned, second.returned);
  CHECK_EQ(first.duplicates_absorbed, second.duplicates_absorbed);
  CHECK_EQ(first.refusals, second.refusals);
  CHECK_EQ(first.operations_completed, second.operations_completed);
}

CF_TEST(the_benchmark_exercises_idempotent_replays) {
  BenchConfig config{};
  config.operations = 30000;
  config.capacity = 1ull << 20;
  const BenchResult result = run_synthetic_ledger_benchmark(config);
  CHECK(result.duplicates_absorbed > 0);
  CHECK(result.closure_ok);
}

CF_TEST(a_durable_benchmark_survives_its_own_journal) {
  BenchConfig config{};
  config.operations = 2000;
  config.capacity = 1ull << 16;
  config.durable = true;
  config.journal_prefix = cftest::scratch_path("bench-durable");
  const BenchResult result = run_synthetic_ledger_benchmark(config);
  CHECK(result.closure_ok);
  CHECK(result.final_view.closed);
  cftest::wipe_scratch(config.journal_prefix);
}

CF_TEST_MAIN()
