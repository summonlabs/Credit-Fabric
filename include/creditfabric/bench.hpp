// Credit Fabric - synthetic credit-ledger benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This benchmark measures completed synthetic ledger decisions per second
// against the in-process authority. It is NOT a physical flow-control
// measurement: there is no NIC, no switch, no fabric and no hardware credit
// mechanism anywhere in this harness. Every reported row is labelled SYNTHETIC
// and every reported run re-verifies accounting closure at the end.

#ifndef CREDITFABRIC_BENCH_HPP
#define CREDITFABRIC_BENCH_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "creditfabric/engine.hpp"
#include "creditfabric/ledger.hpp"

namespace creditfabric {

struct BenchConfig {
  std::string scenario{"synthetic-ledger"};
  std::uint64_t operations{200000};
  std::uint64_t capacity{1ull << 22};
  std::size_t producers{8};
  std::size_t consumers{8};
  std::size_t resources{4};
  std::uint64_t seed{0xC0FFEEull};
  bool durable{false};
  std::string journal_prefix{};
};

struct BenchResult {
  std::string scenario{};
  std::string provenance{"SYNTHETIC"};
  std::uint64_t operations_completed{0};
  std::uint64_t operations_attempted{0};
  std::uint64_t duplicates_absorbed{0};
  std::uint64_t refusals{0};
  std::uint64_t issued{0};
  std::uint64_t consumed{0};
  std::uint64_t returned{0};
  double seconds{0.0};
  double ops_per_second{0.0};
  bool closure_ok{false};
  CreditView final_view{};
  std::string journal{};
  std::vector<std::pair<std::string, std::uint64_t>> refusal_reasons{};
};

/// Records one refusal reason in a bounded tally (top reasons by name).
void tally_refusal(BenchResult& result, Reason reason);

[[nodiscard]] BenchResult run_synthetic_ledger_benchmark(const BenchConfig& config);
[[nodiscard]] std::string render(const BenchResult& result);

}  // namespace creditfabric

#endif  // CREDITFABRIC_BENCH_HPP
