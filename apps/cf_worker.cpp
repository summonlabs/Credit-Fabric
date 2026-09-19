// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// cf_worker - a real OS process that holds no authority of its own.
//
// A worker must reconcile against the coordinator's per-boot challenge before
// any attempt of its own can be admitted. Every round issues, consumes and
// returns the same amount, so a healthy run leaves the account closing exactly
// at capacity with zero in-flight credit. Any refused operation is reported and
// the process exits non-zero: a refusal in this scenario is a defect.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "args.hpp"

#include "creditfabric/creditfabric.hpp"

using namespace creditfabric;

namespace {

int refuse(const char* stage, const Status& status) {
  std::fprintf(stderr, "REFUSED %s: %s\n", stage, status.describe().c_str());
  std::fflush(stderr);
  return 5;
}

}  // namespace

int main(int argc, char** argv) {
  cfapp::Args args(argc, argv);
  bool ok = true;
  const std::uint64_t port = args.number("--port", 0, ok);
  const std::uint64_t domain_value = args.number("--domain", 0xD0A1ull, ok);
  const std::uint64_t account_value = args.number("--account", 0xACCu, ok);
  const std::uint64_t publisher = args.number("--publisher", 0x1001ull, ok);
  const std::uint64_t boot = args.number("--boot", 0, ok);
  const std::uint64_t issue = args.number("--issue", 8, ok);
  const std::uint64_t rounds = args.number("--rounds", 4, ok);
  if (!ok) return cfapp::fail("a numeric option was not a valid unsigned integer");
  if (port == 0) return cfapp::fail("--port is required");
  if (issue == 0) return cfapp::fail("--issue must be greater than zero");

  ClientOptions options{};
  options.host = args.get("--host", "127.0.0.1");
  options.port = static_cast<std::uint16_t>(port);
  options.domain = DomainId{domain_value};
  options.publisher = PublisherId{publisher};
  options.boot = BootId{boot == 0 ? make_boot_id() : boot};
  options.label = args.get("--label", "creditfabric-worker");

  CoordinatorClient client(options);
  const Status connected = client.connect();
  if (!connected.ok()) {
    std::fprintf(stderr, "connect failed: %s\n", connected.describe().c_str());
    return 3;
  }

  const AccountId account{account_value};
  const CreditOutcome revalidated = client.reconcile(account);
  if (!revalidated.ok()) return refuse("reconcile", revalidated.status);
  std::printf("RECONCILED incarnation=%s sequence=%llu\n", to_hex(client.incarnation().id.value()).c_str(),
              static_cast<unsigned long long>(client.next_sequence()));
  std::fflush(stdout);

  if (args.has("--hold")) {
    std::printf("HOLDING\n");
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  for (std::uint64_t round = 0; round < rounds; ++round) {
    CreditRequest grant{};
    grant.authority.account = account;
    grant.op = OpKind::Issue;
    grant.count = issue;
    const CreditOutcome granted = client.submit(grant);
    if (!granted.ok()) return refuse("issue", granted.status);

    CreditRequest spend{};
    spend.authority.account = account;
    spend.op = OpKind::Consume;
    spend.count = issue;
    const CreditOutcome spent = client.submit(spend);
    if (!spent.ok()) return refuse("consume", spent.status);

    CreditRequest give{};
    give.authority.account = account;
    give.op = OpKind::Return;
    give.count = issue;
    const CreditOutcome returned = client.submit(give);
    if (!returned.ok()) return refuse("return", returned.status);
    std::printf("ROUND %llu issued=%llu consumed=%llu returned=%llu\n",
                static_cast<unsigned long long>(round + 1), static_cast<unsigned long long>(granted.view.issued),
                static_cast<unsigned long long>(returned.view.consumed),
                static_cast<unsigned long long>(returned.view.returned));
    std::fflush(stdout);
  }

  const CreditOutcome final_view = client.describe(account);
  if (!final_view.ok()) return refuse("describe", final_view.status);
  std::printf("FINAL capacity=%llu available=%llu issued=%llu consumed=%llu returned=%llu in_flight=%llu closed=%s\n",
              static_cast<unsigned long long>(final_view.view.capacity),
              static_cast<unsigned long long>(final_view.view.available),
              static_cast<unsigned long long>(final_view.view.issued),
              static_cast<unsigned long long>(final_view.view.consumed),
              static_cast<unsigned long long>(final_view.view.returned),
              static_cast<unsigned long long>(final_view.view.in_flight),
              final_view.view.closed ? "yes" : "no");
  std::printf("DONE refusals=%llu duplicates=%llu reconstructions=%llu\n",
              static_cast<unsigned long long>(client.stats().refusals),
              static_cast<unsigned long long>(client.stats().duplicates),
              static_cast<unsigned long long>(client.stats().retries));
  std::fflush(stdout);
  client.close();
  return 0;
}
