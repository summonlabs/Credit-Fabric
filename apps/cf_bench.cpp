// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// cf_bench - synthetic credit-ledger benchmark. SYNTHETIC ONLY: no NIC, no
// switch, no fabric and no hardware credit mechanism participates.

#include <cstdio>
#include <string>

#include "args.hpp"

#include "creditfabric/creditfabric.hpp"

using namespace creditfabric;

int main(int argc, char** argv) {
  cfapp::Args args(argc, argv);
  bool ok = true;
  BenchConfig config{};
  config.scenario = args.get("--scenario", "synthetic-ledger");
  config.operations = args.number("--operations", 200000, ok);
  config.capacity = args.number("--capacity", 1ull << 22, ok);
  config.producers = static_cast<std::size_t>(args.number("--producers", 8, ok));
  config.consumers = static_cast<std::size_t>(args.number("--consumers", 8, ok));
  config.seed = args.number("--seed", 0xC0FFEEull, ok);
  config.durable = args.has("--durable");
  config.journal_prefix = args.get("--journal", "creditfabric-bench");
  if (!ok) return cfapp::fail("a numeric option was not a valid unsigned integer");

  const BenchResult result = run_synthetic_ledger_benchmark(config);
  std::printf("%s", render(result).c_str());
  return result.closure_ok ? 0 : 1;
}
